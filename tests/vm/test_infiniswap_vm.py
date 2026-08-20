import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

VM_DIR = Path(__file__).resolve().parent
if str(VM_DIR) not in sys.path:
    sys.path.insert(0, str(VM_DIR))

import infiniswap_vm  # noqa: E402
import qemu_backend  # noqa: E402


class ScenarioRecordingBackend(qemu_backend.QemuBackend):
    def __init__(self):
        self.helper_calls = []
        self.device_calls = []
        self.waited_for_reboot = False
        self.previous_boot_id = None
        self.transports_reset = False

    def _helper(self, handle, guest, label, *args, **kwargs):
        self.helper_calls.append((label, args, kwargs))

    def _create_device(self, *args, **kwargs):
        self.device_calls.append((args, kwargs))

    def _stop_device(self, handle):
        pass

    def _check_kernel(self, handle):
        pass

    def _ssh_shell(self, handle, guest, label, command, **kwargs):
        if label == "force-reboot":
            raise subprocess.TimeoutExpired("ssh", 15)
        if label.startswith("kernel-release-"):
            return SimpleNamespace(returncode=0, stdout="6.8.0-137-generic\n")
        return SimpleNamespace(returncode=0, stdout="old-boot-id\n")

    def _wait_for_reboot(self, handle, guest, previous_boot_id=None):
        self.waited_for_reboot = True
        self.previous_boot_id = previous_boot_id

    def _reset_provider_guests(self, handle):
        pass

    def _reset_transports(self, handle):
        self.transports_reset = True


class PerformanceRecordingBackend(ScenarioRecordingBackend):
    def __init__(self):
        super().__init__()
        self.args = SimpleNamespace(
            scenario="fio-verification",
            source_commit="a" * 40,
            host_identity="perf-host",
        )

    def _helper(self, handle, guest, label, *args, **kwargs):
        self.helper_calls.append((label, args, kwargs))
        if args[0] == "fio-performance":
            raw_fio = {
                "jobs": [
                    {
                        "error": 0,
                        "read": {
                            "iops": 600.0,
                            "bw_bytes": 2457600,
                            "clat_ns": {"percentile": {"99.000000": 1000}},
                        },
                        "write": {
                            "iops": 400.0,
                            "bw_bytes": 1638400,
                            "clat_ns": {"percentile": {"99.000000": 1200}},
                        },
                        "sys_cpu": 20.0,
                    }
                ]
            }
            arguments = [
                "--runtime=60",
                "--verify=crc32c",
                "--verify_fatal=1",
            ]
            verification_arguments = []
            verification = None
            if args[1] == "remote-only":
                arguments.append("--do_verify=0")
                verification_arguments = [
                    "--rw=read",
                    "--verify=crc32c",
                    "--verify_only=1",
                    "--verify_fatal=1",
                ]
                verification = raw_fio
            else:
                arguments.append("--do_verify=1")
            return SimpleNamespace(
                stdout=json.dumps(
                    {
                        "arguments": arguments,
                        "fio": raw_fio,
                        "verification_arguments": verification_arguments,
                        "verification": verification,
                    }
                )
            )
        if args[0] == "performance-evidence":
            return SimpleNamespace(
                stdout=json.dumps(
                    {
                        "status": "passed",
                        "provider_timeouts_total": 0,
                        "late_rdma_completions_total": 0,
                        "io_errors_total": 0,
                        "inflight_io": 0,
                        "oldest_inflight_ms": 0,
                    }
                )
            )
        return SimpleNamespace(stdout="")


class QemuScenarioTest(unittest.TestCase):
    def test_build_deploy_exercises_and_clears_authentication_alert(self):
        backend = ScenarioRecordingBackend()
        consumer = SimpleNamespace(
            links=[{"provider_ip": "192.0.2.2", "consumer_rail": "rxe0"}]
        )
        handle = SimpleNamespace(
            consumer=consumer,
            providers=[SimpleNamespace(name="provider-0")],
            image_sha256="fixture-sha",
            guests=[consumer, object()],
        )

        result = backend._scenario_build_deploy(handle)

        calls = {label: args for label, args, _kwargs in backend.helper_calls}
        self.assertEqual(result.status, "passed")
        self.assertEqual(result.metrics["kernel_release"], "6.8.0-137-generic")
        self.assertEqual(result.metrics["rdma_stack"], "inbox")
        self.assertFalse(backend.device_calls[0][1]["wait_for_connection"])
        self.assertEqual(
            calls["alert-auth-failure"],
            (
                "assert-alert",
                "auth-failure",
                "InfiniswapAuthenticationFailure",
                "present",
            ),
        )
        self.assertEqual(
            calls["alert-auth-cleared"],
            (
                "assert-alert",
                "auth-cleared",
                "InfiniswapAuthenticationFailure",
                "absent",
            ),
        )

    def test_remote_first_verifies_only_the_write_that_observes_backing_failure(self):
        backend = ScenarioRecordingBackend()
        handle = SimpleNamespace(consumer=object())

        result = backend._scenario_backing_error(handle)

        calls = {label: args for label, args, _kwargs in backend.helper_calls}
        self.assertEqual(result.status, "passed")
        self.assertEqual(
            calls["backing-write"],
            ("write-pattern", "backing-fault", "32", "no-flush", "1"),
        )
        self.assertEqual(
            calls["backing-read"],
            ("read-pattern", "backing-fault", "32", "1"),
        )
        self.assertEqual(
            calls["alert-backing-degraded"],
            (
                "assert-alert",
                "backing-degraded",
                "InfiniswapBackingDegraded",
                "present",
            ),
        )
        self.assertEqual(
            calls["alert-backing-cleared"],
            (
                "assert-alert",
                "backing-cleared",
                "InfiniswapBackingDegraded",
                "absent",
            ),
        )

    def test_remote_chunk_certification_exercises_atomicity_and_mode_policies(self):
        backend = ScenarioRecordingBackend()
        consumer = SimpleNamespace(
            name="consumer", links=[{"consumer_rail": "rxe0"}]
        )
        provider = SimpleNamespace(
            name="provider-0", links=[{"provider_rail": "rxe0"}]
        )
        handle = SimpleNamespace(
            consumer=consumer,
            providers=[provider],
            guests=[consumer, provider],
        )

        result = backend._scenario_remote_chunk_certification(handle)

        calls = {label: args for label, args, _kwargs in backend.helper_calls}
        self.assertEqual(result.status, "passed")
        self.assertEqual(
            [call[0][1] for call in backend.device_calls],
            ["backed", "remote-only"],
        )
        self.assertEqual(
            calls["remote-chunk-contracts"], ("remote-chunk-contracts",)
        )
        self.assertEqual(
            calls["provider-eviction-policy"],
            (
                "provider-start",
                "19420",
                "/etc/infiniswap-vm.psk",
                "provider-0",
                "auto",
                "1",
                "2",
            ),
        )
        self.assertEqual(
            calls["eviction-pressure-start"],
            ("provider-pressure", "start", "backed"),
        )
        self.assertEqual(
            calls["eviction-delay"],
            ("rdma-delay", "add", "rxe0", "1000"),
        )
        self.assertEqual(
            calls["eviction-observer-start"],
            ("observe-eviction", "start", "provider-0"),
        )
        self.assertEqual(
            calls["eviction-observe"], ("observe-eviction", "wait")
        )
        self.assertEqual(
            calls["committed-under-pressure"],
            ("verify-remote-only-committed", "provider-0", "5"),
        )
        self.assertEqual(
            calls["committed-pressure-start"],
            ("provider-pressure", "start", "remote-only"),
        )
        self.assertEqual(
            calls["remote-only-lost"],
            ("wait-field", "connection_state", "remote-lost", "400"),
        )
        self.assertEqual(
            calls["remote-only-operational-lost"],
            ("wait-field", "operational_state", "remote-lost", "20"),
        )
        self.assertEqual(
            calls["remote-only-lost-once"],
            ("wait-field", "remote_lost_transitions_total", "1", "20"),
        )
        self.assertEqual(
            calls["remote-only-io-failure"], ("expect-io-failure",)
        )
        self.assertTrue(backend.transports_reset)
        self.assertIn(
            "guest:consumer:remote-chunk-contracts.json", result.artifacts
        )
        self.assertIn(
            "guest:consumer:atomic-eviction.json", result.artifacts
        )
        self.assertIn(
            "guest:provider-0:provider-pressure-backed.json", result.artifacts
        )
        self.assertIn(
            "guest:provider-0:provider-pressure-remote-only.json",
            result.artifacts,
        )
        self.assertIn(
            "guest:consumer:status-remote-chunk-remote-lost.json",
            result.artifacts,
        )

    def test_remote_only_fio_crosses_multiple_heartbeat_intervals(self):
        backend = ScenarioRecordingBackend()
        handle = SimpleNamespace(consumer=object(), providers=[object()])

        result = backend._scenario_fio(handle)

        calls = {label: args for label, args, _kwargs in backend.helper_calls}
        self.assertEqual(result.status, "passed")
        self.assertEqual(
            calls["heartbeat-remote-only"],
            ("verify-remote-only-heartbeats", "5"),
        )
        self.assertEqual(calls["fio-remote-only"], ("fio", "remote-only"))
        heartbeat_index = next(
            index
            for index, call in enumerate(backend.helper_calls)
            if call[0] == "heartbeat-remote-only"
        )
        fio_index = next(
            index
            for index, call in enumerate(backend.helper_calls)
            if call[0] == "fio-remote-only"
        )
        self.assertLess(heartbeat_index, fio_index)

    def test_focused_fio_retains_warmups_samples_and_environment_identity(self):
        backend = PerformanceRecordingBackend()
        with tempfile.TemporaryDirectory() as directory:
            case_dir = Path(directory)
            handle = SimpleNamespace(
                consumer=object(),
                providers=[object()],
                image_sha256="image-sha256",
                case_dir=case_dir,
                entry={
                    "kernel": "6.8",
                    "ubuntu": "24.04",
                    "topology": 2,
                    "resources": {"vcpus": 6, "memory_gib": 48, "disk_gib": 100},
                    "guests": [
                        {
                            "name": "consumer",
                            "role": "consumer",
                            "vcpus": 3,
                            "memory_gib": 24,
                            "root_disk_gib": 48,
                            "backing_disk_gib": 4,
                        },
                        {
                            "name": "provider-0",
                            "role": "provider",
                            "vcpus": 3,
                            "memory_gib": 24,
                            "root_disk_gib": 48,
                            "backing_disk_gib": 0,
                        },
                    ],
                    "kernel_release": "6.8.0-137-generic",
                    "rdma_stack": "inbox",
                },
            )

            result = backend._scenario_fio(handle)
            envelopes = [
                json.loads(path.read_text(encoding="utf-8"))
                for path in sorted((case_dir / "performance").glob("*.json"))
            ]

        self.assertEqual(result.status, "passed")
        self.assertEqual(len(envelopes), 12)
        for mode in ("backed", "remote-only"):
            selected = [
                envelope
                for envelope in envelopes
                if envelope["metadata"]["mode"] == mode
            ]
            self.assertEqual(
                [envelope["metadata"]["phase"] for envelope in selected].count(
                    "warmup"
                ),
                1,
            )
            self.assertEqual(
                [envelope["metadata"]["phase"] for envelope in selected].count(
                    "measured"
                ),
                5,
            )
        for envelope in envelopes:
            metadata = envelope["metadata"]
            self.assertEqual(metadata["duration_seconds"], 60)
            self.assertEqual(metadata["source_commit"], "a" * 40)
            self.assertEqual(metadata["cloud_image_sha256"], "image-sha256")
            self.assertEqual(metadata["kernel_release"], "6.8.0-137-generic")
            self.assertEqual(metadata["topology"], 2)
            self.assertEqual(metadata["host_identity"], "perf-host")
            self.assertIn("--runtime=60", metadata["fio_arguments"])
            self.assertIn("--verify=crc32c", metadata["fio_arguments"])
            self.assertIn("--verify_fatal=1", metadata["fio_arguments"])
            if metadata["mode"] == "remote-only":
                self.assertIn("--do_verify=0", metadata["fio_arguments"])
                self.assertIn(
                    "--verify_only=1", metadata["fio_verification_arguments"]
                )
            else:
                self.assertIn("--do_verify=1", metadata["fio_arguments"])
                self.assertEqual(metadata["fio_verification_arguments"], [])
            self.assertIn("vm_resources", metadata)
            self.assertIn("fio", envelope)
        self.assertEqual(
            set(result.metrics["evidence"]), {"backed", "remote-only"}
        )
        calls = {label: args for label, args, _kwargs in backend.helper_calls}
        self.assertEqual(
            calls["performance-heartbeat-remote-only"],
            ("verify-remote-only-heartbeats", "5"),
        )

    def test_reboot_command_timeout_still_waits_for_guest_restart(self):
        backend = ScenarioRecordingBackend()
        handle = SimpleNamespace(consumer=SimpleNamespace(links=[]))

        result = backend._scenario_guest_reboot(handle)

        self.assertEqual(result.status, "passed")
        self.assertTrue(backend.waited_for_reboot)
        self.assertEqual(backend.previous_boot_id, "old-boot-id")

    def test_changed_boot_id_completes_reboot_without_observing_ssh_down(self):
        backend = object.__new__(qemu_backend.QemuBackend)
        backend._ssh_shell = mock.Mock(
            return_value=SimpleNamespace(returncode=0, stdout="new-boot-id\n")
        )
        handle = SimpleNamespace()
        guest = object()

        with mock.patch.object(
            qemu_backend.time, "monotonic", side_effect=(0, 0, 181)
        ), mock.patch.object(qemu_backend.time, "sleep"):
            backend._wait_for_reboot(handle, guest, "old-boot-id")



class FixedEnvironment:
    def __init__(self, facts):
        self.facts = facts
        self.inspections = 0

    def inspect(self, artifacts):
        self.inspections += 1
        return self.facts


class FailingValidationBackend:
    def __init__(self):
        self.active = False
        self.starts = 0

    def start(self, entry, artifacts):
        self.active = True
        self.starts += 1
        return {"entry": entry, "artifacts": artifacts}

    def run_scenario(self, handle, scenario_id, artifacts):
        if scenario_id == "network-interruption":
            return infiniswap_vm.ScenarioResult(
                status="failed", detail="injected network mismatch", artifacts=()
            )
        metrics = {}
        if scenario_id == "build-deploy":
            metrics = {
                "kernel_release": handle["entry"]["kernel"] + ".0-fixture",
                "rdma_stack": "inbox",
            }
        return infiniswap_vm.ScenarioResult(
            status="passed",
            detail="fixture passed",
            artifacts=(),
            metrics=metrics,
        )

    def collect(self, handle, artifacts):
        return ()

    def shutdown(self, handle):
        self.active = False

    def cleanup(self, handle, retain):
        if not retain:
            self.active = False

    def leaks(self, handle):
        return ("fixture-vm",) if self.active else ()


class ShutdownFailingBackend(FailingValidationBackend):
    def run_scenario(self, handle, scenario_id, artifacts):
        if scenario_id == "build-deploy":
            return super().run_scenario(handle, scenario_id, artifacts)
        return infiniswap_vm.ScenarioResult(
            status="passed", detail="fixture passed", artifacts=()
        )

    def shutdown(self, handle):
        raise RuntimeError("injected shutdown failure")


class VmValidationCliTest(unittest.TestCase):
    def supported_host(self):
        return FixedEnvironment(
            infiniswap_vm.HostFacts(
                system="Linux",
                machine="x86_64",
                kvm_access=True,
                available_vcpus=32,
                available_memory_gib=256,
                available_disk_gib=500,
                commands={
                    name: True for name in infiniswap_vm.REQUIRED_HOST_COMMANDS
                },
                swap_devices=("/host-swap",),
                protected_modules=("ib_core",),
                image_attachments=(),
            )
        )

    def test_budget_overflow_fails_before_host_or_vm_access(self):
        stdout = io.StringIO()
        stderr = io.StringIO()
        with tempfile.TemporaryDirectory() as directory:
            result = infiniswap_vm.main(
                [
                    "--preflight-only",
                    "--json",
                    "--artifacts",
                    directory,
                    "--memory-gib",
                    "49",
                ],
                stdout=stdout,
                stderr=stderr,
                environment=None,
            )

            report = json.loads(stdout.getvalue())

        self.assertEqual(result, 2)
        self.assertEqual(report["status"], "preflight-failed")
        self.assertEqual(report["preflight"]["checks"][0]["name"], "resource-budget")
        self.assertEqual(report["preflight"]["checks"][0]["status"], "failed")
        self.assertIn("48 GiB", report["preflight"]["checks"][0]["detail"])
        self.assertEqual(stderr.getvalue(), "")

    def test_invalid_guest_layout_fails_before_host_inspection(self):
        stdout = io.StringIO()
        with tempfile.TemporaryDirectory() as directory:
            result = infiniswap_vm.main(
                [
                    "--preflight-only",
                    "--json",
                    "--topology",
                    "3",
                    "--memory-gib",
                    "11",
                    "--artifacts",
                    directory,
                ],
                stdout=stdout,
                stderr=io.StringIO(),
                environment=None,
            )

        report = json.loads(stdout.getvalue())
        checks = {check["name"]: check for check in report["preflight"]["checks"]}
        self.assertEqual(result, 2)
        self.assertEqual(checks["resource-layout"]["status"], "failed")
        self.assertIn("12 GiB", checks["resource-layout"]["detail"])
        self.assertNotIn("host-platform", checks)

    def test_host_preflight_reports_every_missing_resource(self):
        stdout = io.StringIO()
        environment = FixedEnvironment(
            infiniswap_vm.HostFacts(
                system="Linux",
                machine="x86_64",
                kvm_access=False,
                available_vcpus=4,
                available_memory_gib=40,
                available_disk_gib=90,
                commands={"qemu-system-x86_64": False, "qemu-img": False},
                swap_devices=("/swapfile",),
                protected_modules=("ib_core",),
                image_attachments=(),
            )
        )
        with tempfile.TemporaryDirectory() as directory:
            result = infiniswap_vm.main(
                [
                    "--preflight-only",
                    "--json",
                    "--artifacts",
                    directory,
                ],
                stdout=stdout,
                stderr=io.StringIO(),
                environment=environment,
            )

        report = json.loads(stdout.getvalue())
        checks = {check["name"]: check for check in report["preflight"]["checks"]}
        self.assertEqual(result, 2)
        self.assertEqual(report["status"], "preflight-failed")
        self.assertEqual(checks["kvm-access"]["status"], "failed")
        self.assertEqual(checks["host-commands"]["status"], "failed")
        self.assertEqual(checks["host-vcpus"]["status"], "failed")
        self.assertEqual(checks["host-memory"]["status"], "failed")
        self.assertEqual(checks["host-disk"]["status"], "failed")
        self.assertEqual(environment.inspections, 1)

    def test_preflight_writes_the_complete_machine_readable_matrix(self):
        stdout = io.StringIO()
        environment = self.supported_host()
        with tempfile.TemporaryDirectory() as directory:
            result = infiniswap_vm.main(
                [
                    "--preflight-only",
                    "--json",
                    "--artifacts",
                    directory,
                ],
                stdout=stdout,
                stderr=io.StringIO(),
                environment=environment,
            )
            report = json.loads(stdout.getvalue())
            persisted = json.loads(
                (Path(directory) / "report.json").read_text(encoding="utf-8")
            )

        expected_scenarios = {
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
        }
        self.assertEqual(result, 0)
        self.assertEqual(report, persisted)
        self.assertEqual(report["status"], "preflight-passed")
        self.assertRegex(report["source_commit"], r"^[0-9a-f]{40}$")
        self.assertTrue(report["profile"]["certifiable"])
        self.assertNotIn("certification", report)
        self.assertEqual(
            report["selection"],
            {"kernel": "all", "topology": "all", "soak_hours": 24.0},
        )
        self.assertEqual(
            {
                (entry["kernel"], entry["ubuntu"], entry["topology"])
                for entry in report["matrix"]
            },
            {
                ("5.15", "22.04", 2),
                ("5.15", "22.04", 3),
                ("6.8", "24.04", 2),
                ("6.8", "24.04", 3),
            },
        )
        for entry in report["matrix"]:
            self.assertEqual(entry["status"], "planned")
            self.assertEqual(
                {scenario["id"] for scenario in entry["scenarios"]},
                expected_scenarios,
            )
            self.assertTrue(
                all(scenario["status"] == "planned" for scenario in entry["scenarios"])
            )
            self.assertEqual(
                entry["isolation"],
                {
                    "acceleration": "kvm",
                    "cpu_priority": "nice=10",
                    "hugepages": False,
                    "host_rdma": False,
                    "io_priority": "idle",
                    "networking": "qemu-userspace",
                },
            )
            self.assertEqual(len(entry["guests"]), entry["topology"])
            self.assertEqual(entry["guests"][0]["role"], "consumer")
            self.assertEqual(
                sum(guest["vcpus"] for guest in entry["guests"]), 6
            )
            self.assertEqual(
                sum(guest["memory_gib"] for guest in entry["guests"]), 48
            )
            self.assertEqual(
                sum(
                    guest["root_disk_gib"] + guest["backing_disk_gib"]
                    for guest in entry["guests"]
                ),
                100,
            )

    def test_focused_fio_plan_expands_prerequisites_and_is_non_certifiable(self):
        stdout = io.StringIO()
        with tempfile.TemporaryDirectory() as directory:
            result = infiniswap_vm.main(
                [
                    "--preflight-only",
                    "--json",
                    "--scenario",
                    "fio-verification",
                    "--kernel",
                    "6.8",
                    "--topology",
                    "2",
                    "--artifacts",
                    directory,
                ],
                stdout=stdout,
                stderr=io.StringIO(),
                environment=self.supported_host(),
            )
            report = json.loads(stdout.getvalue())

        self.assertEqual(result, 0)
        self.assertFalse(report["profile"]["certifiable"])
        self.assertEqual(
            report["certification"],
            {
                "status": "non-certifiable",
                "reason": "focused Soft-RoCE performance evidence",
            },
        )
        self.assertEqual(report["selection"]["scenario"], "fio-verification")
        self.assertEqual(
            [scenario["id"] for scenario in report["matrix"][0]["scenarios"]],
            ["build-deploy", "fio-verification", "resource-leak-check"],
        )

    def test_focused_remote_chunk_plan_expands_prerequisites(self):
        stdout = io.StringIO()
        with tempfile.TemporaryDirectory() as directory:
            result = infiniswap_vm.main(
                [
                    "--preflight-only",
                    "--json",
                    "--scenario",
                    "remote-chunk-certification",
                    "--kernel",
                    "6.8",
                    "--topology",
                    "2",
                    "--artifacts",
                    directory,
                ],
                stdout=stdout,
                stderr=io.StringIO(),
                environment=self.supported_host(),
            )
            report = json.loads(stdout.getvalue())

        self.assertEqual(result, 0)
        self.assertFalse(report["profile"]["certifiable"])
        self.assertEqual(
            report["certification"],
            {
                "status": "non-certifiable",
                "reason": "focused Soft-RoCE Remote Chunk evidence",
            },
        )
        self.assertEqual(
            [scenario["id"] for scenario in report["matrix"][0]["scenarios"]],
            [
                "build-deploy",
                "remote-chunk-certification",
                "resource-leak-check",
            ],
        )

    def test_failure_still_cleans_resources_and_records_skipped_scenarios(self):
        stdout = io.StringIO()
        environment = self.supported_host()
        backend = FailingValidationBackend()
        with tempfile.TemporaryDirectory() as directory:
            result = infiniswap_vm.main(
                [
                    "--kernel",
                    "5.15",
                    "--topology",
                    "2",
                    "--soak-hours",
                    "0",
                    "--memory-gib",
                    "8",
                    "--disk-gib",
                    "24",
                    "--json",
                    "--artifacts",
                    directory,
                ],
                stdout=stdout,
                stderr=io.StringIO(),
                environment=environment,
                backend=backend,
            )
            report = json.loads(stdout.getvalue())

        scenarios = {
            scenario["id"]: scenario for scenario in report["matrix"][0]["scenarios"]
        }
        self.assertEqual(result, 1)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["matrix"][0]["kernel_release"], "5.15.0-fixture")
        self.assertEqual(report["matrix"][0]["rdma_stack"], "inbox")
        self.assertEqual(scenarios["network-interruption"]["status"], "failed")
        self.assertEqual(scenarios["backing-store-error"]["status"], "skipped")
        self.assertEqual(scenarios["resource-leak-check"]["status"], "passed")
        self.assertEqual(report["cleanup"]["status"], "passed")
        self.assertEqual(report["safety"]["status"], "passed")
        self.assertFalse(backend.active)

    def test_keep_on_failure_retains_one_case_and_stops_the_matrix(self):
        stdout = io.StringIO()
        backend = FailingValidationBackend()
        with tempfile.TemporaryDirectory() as directory:
            result = infiniswap_vm.main(
                [
                    "--soak-hours",
                    "0",
                    "--memory-gib",
                    "12",
                    "--disk-gib",
                    "28",
                    "--keep-on-failure",
                    "--json",
                    "--artifacts",
                    directory,
                ],
                stdout=stdout,
                stderr=io.StringIO(),
                environment=self.supported_host(),
                backend=backend,
            )
            report = json.loads(stdout.getvalue())

        self.assertEqual(result, 1)
        self.assertEqual(backend.starts, 1)
        self.assertTrue(backend.active)
        self.assertEqual(report["matrix"][0]["status"], "failed")
        first_scenarios = {
            scenario["id"]: scenario for scenario in report["matrix"][0]["scenarios"]
        }
        self.assertEqual(first_scenarios["resource-leak-check"]["status"], "retained")
        for entry in report["matrix"][1:]:
            self.assertEqual(entry["status"], "skipped")
            self.assertTrue(
                all(scenario["status"] == "skipped" for scenario in entry["scenarios"])
            )

    def test_cleanup_runs_even_when_normal_shutdown_fails(self):
        stdout = io.StringIO()
        backend = ShutdownFailingBackend()
        with tempfile.TemporaryDirectory() as directory:
            result = infiniswap_vm.main(
                [
                    "--kernel",
                    "6.8",
                    "--topology",
                    "2",
                    "--soak-hours",
                    "0",
                    "--memory-gib",
                    "8",
                    "--disk-gib",
                    "24",
                    "--json",
                    "--artifacts",
                    directory,
                ],
                stdout=stdout,
                stderr=io.StringIO(),
                environment=self.supported_host(),
                backend=backend,
            )
            report = json.loads(stdout.getvalue())

        self.assertEqual(result, 1)
        self.assertEqual(report["cleanup"]["status"], "failed")
        self.assertIn("shutdown", report["cleanup"]["detail"])
        self.assertEqual(
            report["matrix"][0]["scenarios"][-1]["status"], "passed"
        )
        self.assertFalse(backend.active)


if __name__ == "__main__":
    unittest.main()
