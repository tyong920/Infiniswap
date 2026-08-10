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
        self.waited_for_reboot = False
        self.previous_boot_id = None

    def _helper(self, handle, guest, label, *args, **kwargs):
        self.helper_calls.append((label, args, kwargs))

    def _create_device(self, *args, **kwargs):
        pass

    def _stop_device(self, handle):
        pass

    def _check_kernel(self, handle):
        pass

    def _ssh_shell(self, handle, guest, label, command, **kwargs):
        if label == "force-reboot":
            raise subprocess.TimeoutExpired("ssh", 15)
        return SimpleNamespace(returncode=0, stdout="old-boot-id\n")

    def _wait_for_reboot(self, handle, guest, previous_boot_id=None):
        self.waited_for_reboot = True
        self.previous_boot_id = previous_boot_id

    def _reset_provider_guests(self, handle):
        pass


class QemuScenarioTest(unittest.TestCase):
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
        return infiniswap_vm.ScenarioResult(
            status="passed", detail="fixture passed", artifacts=()
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
        self.assertTrue(report["profile"]["certifiable"])
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
