#!/usr/bin/env python3
"""Host-safe QEMU validation harness for Infiniswap."""

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Optional, Sequence, TextIO, Tuple

GIB = 1024 ** 3


@dataclass(frozen=True)
class Resources:
    vcpus: int
    memory_gib: int
    disk_gib: int


PROFILES = {
    "default": Resources(vcpus=6, memory_gib=48, disk_gib=100),
    "large": Resources(vcpus=16, memory_gib=128, disk_gib=160),
}

DEFAULT_SCENARIOS = (
    "build-deploy",
    "fio-verification",
    "swap-pressure",
    "normal-shutdown",
    "remote-chunk-certification",
    "provider-process-kill",
    "network-interruption",
    "backing-store-error",
    "guest-reboot",
    "safe-module-reload",
    "resource-leak-check",
    "soak",
)
SCENARIO_PREREQUISITES = {
    "fio-verification": ("build-deploy",),
    "remote-chunk-certification": ("build-deploy",),
}
CLEANUP_SCENARIO = "resource-leak-check"

REQUIRED_HOST_COMMANDS = (
    "cloud-localds",
    "git",
    "ionice",
    "nice",
    "qemu-img",
    "qemu-system-x86_64",
    "scp",
    "ssh",
    "ssh-keygen",
)
PROTECTED_HOST_MODULES = ("infiniswap", "rdma_rxe", "ib_core", "rdma_cm")
HOST_MEMORY_RESERVE_GIB = 2
HOST_DISK_RESERVE_GIB = 8


@dataclass(frozen=True)
class HostFacts:
    system: str
    machine: str
    kvm_access: bool
    available_vcpus: int
    available_memory_gib: int
    available_disk_gib: int
    commands: Dict[str, bool]
    swap_devices: Tuple[str, ...]
    protected_modules: Tuple[str, ...]
    image_attachments: Tuple[str, ...]


@dataclass(frozen=True)
class ScenarioResult:
    status: str
    detail: str
    artifacts: Tuple[str, ...] = ()
    metrics: Dict[str, object] = field(default_factory=dict)


class LocalEnvironment:
    """Read-only access to host state used by preflight and safety checks."""

    def inspect(self, artifacts: Path) -> HostFacts:
        existing = artifacts.resolve()
        while not existing.exists() and existing != existing.parent:
            existing = existing.parent
        disk = shutil.disk_usage(str(existing))
        memory_gib = 0
        meminfo = Path("/proc/meminfo")
        if meminfo.exists():
            for line in meminfo.read_text(encoding="ascii").splitlines():
                if line.startswith("MemAvailable:"):
                    memory_gib = int(line.split()[1]) * 1024 // GIB
                    break
        swaps = []
        proc_swaps = Path("/proc/swaps")
        if proc_swaps.exists():
            swaps = [
                line.split()[0]
                for line in proc_swaps.read_text(encoding="utf-8").splitlines()[1:]
                if line.split()
            ]
        modules = tuple(
            name for name in PROTECTED_HOST_MODULES if Path("/sys/module", name).is_dir()
        )
        attachments = []
        for path in sorted(Path("/sys/class/block").glob("nbd*/pid")):
            try:
                pid = path.read_text(encoding="ascii").strip()
            except OSError:
                continue
            if pid:
                attachments.append("%s:%s" % (path.parent.name, pid))
        return HostFacts(
            system=platform.system(),
            machine=platform.machine(),
            kvm_access=os.access("/dev/kvm", os.R_OK | os.W_OK),
            available_vcpus=os.cpu_count() or 0,
            available_memory_gib=memory_gib,
            available_disk_gib=disk.free // GIB,
            commands={name: shutil.which(name) is not None for name in REQUIRED_HOST_COMMANDS},
            swap_devices=tuple(swaps),
            protected_modules=modules,
            image_attachments=tuple(attachments),
        )


def _default_artifacts() -> Path:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return Path("results") / "vm" / ("%s-%d" % (stamp, os.getpid()))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run Infiniswap in disposable Ubuntu KVM guests"
    )
    parser.add_argument("--large", action="store_true", help="use the large budget")
    parser.add_argument("--vcpus", type=int)
    parser.add_argument("--memory-gib", type=int)
    parser.add_argument("--disk-gib", type=int)
    parser.add_argument(
        "--kernel", choices=("all", "5.15", "6.8"), default="all"
    )
    parser.add_argument("--topology", choices=("all", "2", "3"), default="all")
    parser.add_argument(
        "--scenario",
        choices=("all", "fio-verification", "remote-chunk-certification"),
        default="all",
        help="run the default certifiable plan or focused evidence",
    )
    parser.add_argument("--soak-hours", type=float, default=24.0)
    parser.add_argument("--artifacts", type=Path, default=_default_artifacts())
    parser.add_argument(
        "--cache-dir",
        type=Path,
        default=Path.home() / ".cache" / "infiniswap-vm",
        help="verified Ubuntu cloud image cache",
    )
    parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--keep-on-failure", action="store_true")
    parser.add_argument("--json", action="store_true")
    return parser


def _budget_check(requested: Resources, limit: Resources):
    overflow = []
    if requested.vcpus > limit.vcpus:
        overflow.append("%d vCPU (limit %d)" % (requested.vcpus, limit.vcpus))
    if requested.memory_gib > limit.memory_gib:
        overflow.append(
            "%d GiB RAM (limit %d GiB)" % (requested.memory_gib, limit.memory_gib)
        )
    if requested.disk_gib > limit.disk_gib:
        overflow.append(
            "%d GiB disk (limit %d GiB)" % (requested.disk_gib, limit.disk_gib)
        )
    if overflow:
        return {
            "name": "resource-budget",
            "status": "failed",
            "detail": "requested resources exceed profile: " + ", ".join(overflow),
        }
    return {
        "name": "resource-budget",
        "status": "passed",
        "detail": "requested resources are within the aggregate profile",
    }


def _layout_check(args, requested: Resources):
    largest_topology = 3 if args.topology in ("all", "3") else 2
    minimum_memory = 4 * largest_topology
    minimum_disk = 4 + 8 * largest_topology
    failures = []
    if requested.vcpus < largest_topology:
        failures.append("at least %d vCPU" % largest_topology)
    if requested.memory_gib < minimum_memory:
        failures.append("at least %d GiB RAM" % minimum_memory)
    if requested.disk_gib < minimum_disk:
        failures.append("at least %d GiB sparse disk" % minimum_disk)
    if args.soak_hours < 0:
        failures.append("a non-negative soak duration")
    return {
        "name": "resource-layout",
        "status": "failed" if failures else "passed",
        "detail": "guest layout requires " + ", ".join(failures)
        if failures
        else "each guest has a vCPU, 4 GiB RAM, and 8 GiB root disk",
    }


def _host_checks(facts: HostFacts, requested: Resources):
    missing = sorted(name for name, present in facts.commands.items() if not present)
    supported_platform = facts.system == "Linux" and facts.machine == "x86_64"
    checks = [
        {
            "name": "host-platform",
            "status": "passed" if supported_platform else "failed",
            "detail": "%s/%s" % (facts.system, facts.machine),
        },
        {
            "name": "kvm-access",
            "status": "passed" if facts.kvm_access else "failed",
            "detail": "/dev/kvm is readable and writable"
            if facts.kvm_access
            else "/dev/kvm is not readable and writable",
        },
        {
            "name": "host-commands",
            "status": "passed" if not missing else "failed",
            "detail": "all required commands are available"
            if not missing
            else "missing commands: " + ", ".join(missing),
        },
        {
            "name": "host-vcpus",
            "status": "passed"
            if facts.available_vcpus >= requested.vcpus
            else "failed",
            "detail": "%d available, %d requested"
            % (facts.available_vcpus, requested.vcpus),
        },
        {
            "name": "host-memory",
            "status": "passed"
            if facts.available_memory_gib
            >= requested.memory_gib + HOST_MEMORY_RESERVE_GIB
            else "failed",
            "detail": "%d GiB available, %d GiB guests plus %d GiB host reserve required"
            % (
                facts.available_memory_gib,
                requested.memory_gib,
                HOST_MEMORY_RESERVE_GIB,
            ),
        },
        {
            "name": "host-disk",
            "status": "passed"
            if facts.available_disk_gib >= requested.disk_gib + HOST_DISK_RESERVE_GIB
            else "failed",
            "detail": "%d GiB available, %d GiB sparse capacity plus %d GiB cache/artifact reserve required"
            % (
                facts.available_disk_gib,
                requested.disk_gib,
                HOST_DISK_RESERVE_GIB,
            ),
        },
        {
            "name": "host-safety-baseline",
            "status": "passed",
            "detail": "swap=%s modules=%s attachments=%s"
            % (
                ",".join(facts.swap_devices) or "none",
                ",".join(facts.protected_modules) or "none",
                ",".join(facts.image_attachments) or "none",
            ),
        },
    ]
    return checks


def _distribute(total: int, count: int):
    base, remainder = divmod(total, count)
    return [base + (1 if index < remainder else 0) for index in range(count)]


def _guest_plan(topology: int, requested: Resources):
    backing_gib = 4
    root_total = requested.disk_gib - backing_gib
    vcpus = _distribute(requested.vcpus, topology)
    memory = _distribute(requested.memory_gib, topology)
    root_disks = _distribute(root_total, topology)
    guests = []
    for index in range(topology):
        guests.append(
            {
                "name": "consumer" if index == 0 else "provider-%d" % (index - 1),
                "role": "consumer" if index == 0 else "provider",
                "vcpus": vcpus[index],
                "memory_gib": memory[index],
                "root_disk_gib": root_disks[index],
                "backing_disk_gib": backing_gib if index == 0 else 0,
            }
        )
    return guests


def _selected_scenarios(selection: str):
    if selection == "all":
        return DEFAULT_SCENARIOS
    scenarios = []

    def include(scenario_id: str) -> None:
        for prerequisite in SCENARIO_PREREQUISITES.get(scenario_id, ()):
            include(prerequisite)
        if scenario_id not in scenarios:
            scenarios.append(scenario_id)

    include(selection)
    include(CLEANUP_SCENARIO)
    return tuple(scenarios)


def _planned_matrix(args, requested: Resources):
    kernels = {
        "5.15": "22.04",
        "6.8": "24.04",
    }
    selected_kernels = kernels if args.kernel == "all" else {args.kernel: kernels[args.kernel]}
    topologies = (2, 3) if args.topology == "all" else (int(args.topology),)
    scenario_ids = _selected_scenarios(args.scenario)
    matrix = []
    for kernel, ubuntu in selected_kernels.items():
        for topology in topologies:
            matrix.append(
                {
                    "kernel": kernel,
                    "ubuntu": ubuntu,
                    "topology": topology,
                    "status": "planned",
                    "resources": asdict(requested),
                    "isolation": {
                        "acceleration": "kvm",
                        "networking": "qemu-userspace",
                        "cpu_priority": "nice=10",
                        "io_priority": "idle",
                        "hugepages": False,
                        "host_rdma": False,
                    },
                    "guests": _guest_plan(topology, requested),
                    "scenarios": [
                        {"id": scenario_id, "status": "planned"}
                        for scenario_id in scenario_ids
                    ],
                }
            )
    return matrix


def _persist_report(path: Path, report) -> None:
    path.mkdir(parents=True, exist_ok=True)
    temporary = path / "report.json.tmp"
    temporary.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path / "report.json")


def _source_commit() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=Path(__file__).resolve().parents[2],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return result.stdout.strip()


def _utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def _host_safety(before: HostFacts, after: HostFacts):
    changes = []
    if before.swap_devices != after.swap_devices:
        changes.append(
            "host swap changed from %s to %s"
            % (before.swap_devices, after.swap_devices)
        )
    if before.protected_modules != after.protected_modules:
        changes.append(
            "protected host modules changed from %s to %s"
            % (before.protected_modules, after.protected_modules)
        )
    if before.image_attachments != after.image_attachments:
        changes.append(
            "host image attachments changed from %s to %s"
            % (before.image_attachments, after.image_attachments)
        )
    return {
        "status": "failed" if changes else "passed",
        "detail": "; ".join(changes)
        if changes
        else "host swap, protected modules, and image attachments are unchanged",
    }


def _run_matrix(args, report, environment, initial_facts, backend) -> int:
    any_failure = False
    cleanup_failures = []
    report["started_at"] = _utc_now()
    report["status"] = "running"
    _persist_report(args.artifacts, report)

    for entry_index, entry in enumerate(report["matrix"]):
        entry["started_at"] = _utc_now()
        entry_started = time.monotonic()
        entry["status"] = "running"
        handle = None
        entry_failed = False
        current_scenario = None
        try:
            handle = backend.start(entry, args.artifacts / ("case-%d" % entry_index))
            for scenario in entry["scenarios"]:
                if scenario["id"] == "resource-leak-check":
                    continue
                if entry_failed:
                    scenario["status"] = "skipped"
                    scenario["detail"] = "an earlier scenario failed"
                    continue
                current_scenario = scenario
                scenario["started_at"] = _utc_now()
                scenario_started = time.monotonic()
                try:
                    outcome = backend.run_scenario(
                        handle,
                        scenario["id"],
                        args.artifacts / ("case-%d" % entry_index),
                    )
                except Exception as error:  # System boundary failures become evidence.
                    outcome = ScenarioResult(
                        status="failed",
                        detail="%s: %s" % (type(error).__name__, error),
                    )
                scenario.update(asdict(outcome))
                if scenario["id"] == "build-deploy" and scenario["status"] == "passed":
                    kernel_release = scenario["metrics"].get("kernel_release")
                    rdma_stack = scenario["metrics"].get("rdma_stack")
                    if not kernel_release or rdma_stack != "inbox":
                        scenario["status"] = "failed"
                        scenario["detail"] = (
                            "build evidence omitted the exact kernel ABI or RDMA stack"
                        )
                    else:
                        entry["kernel_release"] = kernel_release
                        entry["rdma_stack"] = rdma_stack
                scenario["finished_at"] = _utc_now()
                scenario["duration_seconds"] = round(
                    time.monotonic() - scenario_started, 3
                )
                if scenario["status"] != "passed":
                    entry_failed = True
                    any_failure = True
                _persist_report(args.artifacts, report)
        except Exception as error:
            entry_failed = True
            any_failure = True
            if current_scenario is None:
                current_scenario = entry["scenarios"][0]
            current_scenario.update(
                {
                    "status": "failed",
                    "detail": "%s: %s" % (type(error).__name__, error),
                    "artifacts": [],
                    "metrics": {},
                }
            )
            for scenario in entry["scenarios"]:
                if scenario["status"] == "planned" and scenario is not current_scenario:
                    scenario["status"] = "skipped"
                    scenario["detail"] = "VM setup failed"
        finally:
            retained = args.keep_on_failure and entry_failed
            collected = ()
            if handle is not None:
                try:
                    collected = backend.collect(
                        handle, args.artifacts / ("case-%d" % entry_index)
                    )
                except Exception as error:
                    cleanup_failures.append("artifact collection: %s" % error)
                if not retained:
                    try:
                        backend.shutdown(handle)
                    except Exception as error:
                        cleanup_failures.append("shutdown: %s" % error)
                try:
                    backend.cleanup(handle, retained)
                except Exception as error:
                    cleanup_failures.append("cleanup: %s" % error)
                try:
                    leaks = tuple(backend.leaks(handle))
                except Exception as error:
                    leaks = ("leak check failed: %s" % error,)
            else:
                leaks = ()
            leak_scenario = next(
                scenario
                for scenario in entry["scenarios"]
                if scenario["id"] == "resource-leak-check"
            )
            leak_scenario["artifacts"] = list(collected)
            leak_scenario["metrics"] = {"leaks": list(leaks)}
            if retained:
                leak_scenario["status"] = "retained"
                leak_scenario["detail"] = "resources retained by --keep-on-failure"
            elif leaks:
                leak_scenario["status"] = "failed"
                leak_scenario["detail"] = "leaked resources: " + ", ".join(leaks)
                entry_failed = True
                any_failure = True
            else:
                leak_scenario["status"] = "passed"
                leak_scenario["detail"] = "no VM, socket, image attachment, or process leak"
            entry["status"] = "failed" if entry_failed else "passed"
            entry["finished_at"] = _utc_now()
            entry["duration_seconds"] = round(time.monotonic() - entry_started, 3)
            _persist_report(args.artifacts, report)
        if retained:
            for remaining in report["matrix"][entry_index + 1 :]:
                remaining["status"] = "skipped"
                remaining["detail"] = "a failed case was retained for inspection"
                for scenario in remaining["scenarios"]:
                    scenario["status"] = "skipped"
                    scenario["detail"] = "a failed case was retained for inspection"
            break

    final_facts = environment.inspect(args.artifacts)
    report["safety"] = _host_safety(initial_facts, final_facts)
    if report["safety"]["status"] == "failed":
        any_failure = True
    report["cleanup"] = {
        "status": "failed" if cleanup_failures else "passed",
        "detail": "; ".join(cleanup_failures)
        if cleanup_failures
        else "all non-retained resources cleaned",
    }
    if cleanup_failures:
        any_failure = True
    report["finished_at"] = _utc_now()
    report["status"] = "failed" if any_failure else "passed"
    _persist_report(args.artifacts, report)
    return 1 if any_failure else 0


def main(
    argv: Optional[Sequence[str]] = None,
    *,
    stdout: TextIO = sys.stdout,
    stderr: TextIO = sys.stderr,
    environment=None,
    backend=None,
) -> int:
    args = _parser().parse_args(argv)
    profile_name = "large" if args.large else "default"
    limit = PROFILES[profile_name]
    requested = Resources(
        vcpus=args.vcpus if args.vcpus is not None else limit.vcpus,
        memory_gib=(
            args.memory_gib if args.memory_gib is not None else limit.memory_gib
        ),
        disk_gib=args.disk_gib if args.disk_gib is not None else limit.disk_gib,
    )
    budget = _budget_check(requested, limit)
    layout = _layout_check(args, requested)
    checks = [budget, layout]
    initial_facts = None
    if budget["status"] == "passed" and layout["status"] == "passed":
        environment = environment or LocalEnvironment()
        initial_facts = environment.inspect(args.artifacts)
        checks.extend(_host_checks(initial_facts, requested))
    preflight_status = (
        "failed" if any(check["status"] == "failed" for check in checks) else "passed"
    )
    report = {
        "schema_version": 1,
        "source_commit": _source_commit(),
        "run_id": args.artifacts.name,
        "artifacts": str(args.artifacts.resolve()),
        "status": "preflight-failed"
        if preflight_status == "failed"
        else "preflight-passed",
        "profile": {
            "name": profile_name,
            "limits": asdict(limit),
            "requested": asdict(requested),
            "certifiable": (
                requested == limit
                and args.soak_hours >= 24.0
                and args.scenario == "all"
            ),
        },
        "selection": {
            "kernel": args.kernel,
            "topology": args.topology,
            "soak_hours": args.soak_hours,
        },
        "preflight": {"status": preflight_status, "checks": checks},
        "matrix": _planned_matrix(args, requested),
    }
    if args.scenario != "all":
        reason = (
            "focused Soft-RoCE performance evidence"
            if args.scenario == "fio-verification"
            else "focused Soft-RoCE Remote Chunk evidence"
        )
        report["selection"]["scenario"] = args.scenario
        report["certification"] = {
            "status": "non-certifiable",
            "reason": reason,
        }
    _persist_report(args.artifacts, report)
    if preflight_status == "passed" and not args.preflight_only:
        if backend is None:
            from qemu_backend import QemuBackend

            args.source_commit = report["source_commit"]
            args.host_identity = platform.node()
            backend = QemuBackend(args, Path(__file__).resolve().parents[2])
        result = _run_matrix(args, report, environment, initial_facts, backend)
    else:
        result = 2 if preflight_status == "failed" else 0
    if args.json:
        json.dump(report, stdout, sort_keys=True)
        stdout.write("\n")
    elif preflight_status == "failed":
        failed = [check["detail"] for check in checks if check["status"] == "failed"]
        stderr.write("preflight failed: %s\n" % "; ".join(failed))
    elif args.preflight_only:
        if args.scenario == "all":
            stdout.write("preflight passed\n")
        else:
            stdout.write("preflight passed; NON-CERTIFIABLE focused evidence\n")
    else:
        prefix = (
            "NON-CERTIFIABLE focused validation"
            if args.scenario != "all"
            else "validation"
        )
        stdout.write(
            "%s %s; report: %s\n"
            % (prefix, report["status"], args.artifacts / "report.json")
        )
    return result


if __name__ == "__main__":
    raise SystemExit(main())
