import io
import json
import stat
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from infiniswapctl.cli import run


GIB = 1024 * 1024 * 1024


class FakeSystem:
    def __init__(self):
        self.mutations = []
        self.root = True
        self.groups = set()
        self.attributes = {}
        self.swap = {"/dev/nvme0n1p2": -2}
        self.swap_signatures = set()
        self.mounted = set()
        self.fail_attr = None
        self.block_devices = {"/dev/vdb", "/dev/infiniswap0"}
        self.file_stats = {
            "/etc/infiniswap/keys/provider-a.psk": SimpleNamespace(
                st_mode=stat.S_IFREG | 0o600, st_uid=0
            ),
            "/etc/infiniswap/keys/consumer-a.psk": SimpleNamespace(
                st_mode=stat.S_IFREG | 0o600, st_uid=0
            ),
        }
        self.rdma_rails = {("mlx5_ib2", 1): 0}

    def is_block_device(self, path):
        return path in self.block_devices

    def resolve_path(self, path):
        return path

    def is_loop_device(self, path):
        return False

    def stat_file(self, path):
        return self.file_stats[path]

    def rdma_rail_numa_node(self, device, port):
        return self.rdma_rails[(device, port)]

    def is_root(self):
        return self.root

    def group_exists(self, name):
        return name in self.groups

    def ensure_configfs(self):
        self.mutations.append(("ensure_configfs",))

    def ensure_module(self):
        self.mutations.append(("ensure_module",))

    def create_group(self, name):
        self.mutations.append(("create_group", name))
        self.groups.add(name)
        self.attributes[name] = {}

    def write_attribute(self, name, attribute, value):
        if attribute == self.fail_attr:
            raise OSError("injected write failure")
        self.mutations.append(("write_attribute", name, attribute, value))
        self.attributes.setdefault(name, {})[attribute] = value

    def read_attribute(self, name, attribute):
        return self.attributes[name][attribute]

    def remove_group(self, name):
        self.mutations.append(("remove_group", name))
        self.groups.remove(name)
        self.attributes.pop(name, None)

    def device_path(self, name):
        return "/dev/" + name

    def is_swap_enabled(self, path):
        return path in self.swap

    def swap_priority(self, path):
        return self.swap.get(path)

    def has_swap_signature(self, path):
        return path in self.swap_signatures

    def is_mounted(self, path):
        return path in self.mounted

    def run_host_command(self, command):
        self.mutations.append(("run_host_command", tuple(command)))
        path = command[-1]
        if command[0] == "mkswap":
            self.swap_signatures.add(path)
        elif command[0] == "swapon":
            self.swap[path] = int(command[2])
        elif command[0] == "swapoff":
            self.swap.pop(path, None)


class CliValidationTest(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary_directory.name)
        self.provider_directory_path = self.directory / "providers.json"
        self.provider_directory_path.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "providers": {
                        "provider-a": {
                            "address": "192.0.2.10",
                            "port": 9400,
                            "rdma_rail": {
                                "device": "mlx5_ib2",
                                "port": 1,
                                "numa_node": 0,
                            },
                            "authentication": {
                                "consumer_id": "consumer-a",
                                "key_id": "key-current",
                                "psk_file": "/etc/infiniswap/keys/provider-a.psk",
                            },
                            "expected_capabilities": [
                                "backed",
                                "opportunistic_pool",
                                "provider_failure_deadline",
                                "status",
                                "auth_hmac_sha256",
                            ],
                            "placement_weight": 100,
                        },
                    },
                }
            ),
            encoding="utf-8",
        )

    def tearDown(self):
        self.temporary_directory.cleanup()

    def write_consumer(self, **device_overrides):
        device = {
            "name": "infiniswap0",
            "mode": "backed",
            "acknowledgement_policy": "strict",
            "backing_store": "/dev/vdb",
            "capacity_bytes": 8 * GIB,
            "provider_failure_deadline_ms": 2000,
            "provider_directory": str(self.provider_directory_path),
            "providers": ["provider-a"],
            "swap_priority": 100,
        }
        device.update(device_overrides)
        config = {
            "schema_version": 1,
            "identity": {"consumer_id": "consumer-a"},
            "device": device,
        }
        path = self.directory / "consumer.json"
        path.write_text(json.dumps(config), encoding="utf-8")
        return path

    def invoke_create(self, path, system=None):
        stdout = io.StringIO()
        stderr = io.StringIO()
        system = system or FakeSystem()
        result = run(
            ["create", "--config", str(path), "--dry-run"],
            system=system,
            stdout=stdout,
            stderr=stderr,
        )
        return result, stdout.getvalue(), stderr.getvalue(), system

    def test_create_rejects_missing_capacity_before_system_mutation(self):
        path = self.write_consumer()
        document = json.loads(path.read_text(encoding="utf-8"))
        del document["device"]["capacity_bytes"]
        path.write_text(json.dumps(document), encoding="utf-8")

        result, stdout, stderr, system = self.invoke_create(path)

        self.assertEqual(result, 2)
        self.assertIn("device.capacity_bytes is required", stderr)
        self.assertEqual(system.mutations, [])
        self.assertEqual(stdout, "")

    def test_create_rejects_untrusted_provider_before_system_mutation(self):
        path = self.write_consumer(providers=["provider-a", "provider-z"])

        result, stdout, stderr, system = self.invoke_create(path)

        self.assertEqual(result, 2)
        self.assertIn("device.providers contains untrusted Provider provider-z", stderr)
        self.assertEqual(system.mutations, [])
        self.assertEqual(stdout, "")

    def test_create_rejects_unsafe_mode_path_capacity_and_deadline(self):
        cases = (
            ({"mode": "remote-only"}, "remote-only is not available"),
            ({"backing_store": "relative-device"}, "must be an absolute path"),
            ({"capacity_bytes": GIB + 512}, "must be aligned to 1 GiB"),
            ({"provider_failure_deadline_ms": 499}, "must be between 500 and 30000"),
        )
        for overrides, message in cases:
            with self.subTest(message=message):
                result, stdout, stderr, system = self.invoke_create(
                    self.write_consumer(**overrides)
                )
                self.assertEqual(result, 2)
                self.assertEqual(stdout, "")
                self.assertIn(message, stderr)
                self.assertEqual(system.mutations, [])

    def test_create_rejects_provider_selection_too_large_for_configfs(self):
        directory = json.loads(self.provider_directory_path.read_text(encoding="utf-8"))
        template = directory["providers"]["provider-a"]
        provider_names = ["p%062d" % index for index in range(64)]
        directory["providers"] = {
            name: json.loads(json.dumps(template)) for name in provider_names
        }
        self.provider_directory_path.write_text(json.dumps(directory), encoding="utf-8")

        result, stdout, stderr, system = self.invoke_create(
            self.write_consumer(providers=provider_names)
        )

        self.assertEqual(result, 2)
        self.assertEqual(stdout, "")
        self.assertIn("Provider selection is too large for configfs", stderr)
        self.assertEqual(system.mutations, [])

    def test_create_dry_run_reports_validated_plan_without_mutation(self):
        path = self.write_consumer()

        result, stdout, stderr, system = self.invoke_create(path)

        self.assertEqual(result, 0)
        self.assertEqual(stderr, "")
        self.assertIn("Preflight create infiniswap0", stdout)
        self.assertIn("mode: backed", stdout)
        self.assertIn("acknowledgement policy: strict", stdout)
        self.assertIn("Providers: provider-a", stdout)
        self.assertIn("Dry run: no changes made", stdout)
        self.assertEqual(system.mutations, [])

    def test_create_configures_and_activates_one_validated_device(self):
        path = self.write_consumer()
        stdout = io.StringIO()
        stderr = io.StringIO()
        system = FakeSystem()

        result = run(
            ["create", "--config", str(path)],
            system=system,
            stdout=stdout,
            stderr=stderr,
        )

        self.assertEqual(result, 0)
        self.assertEqual(stderr.getvalue(), "")
        self.assertIn("Created active Infiniswap Device infiniswap0", stdout.getvalue())
        self.assertEqual(
            system.mutations,
            [
                ("ensure_configfs",),
                ("ensure_module",),
                ("create_group", "infiniswap0"),
                ("write_attribute", "infiniswap0", "mode", "backed"),
                ("write_attribute", "infiniswap0", "acknowledgement_policy", "strict"),
                ("write_attribute", "infiniswap0", "backing_store", "/dev/vdb"),
                ("write_attribute", "infiniswap0", "capacity_bytes", str(8 * GIB)),
                (
                    "write_attribute",
                    "infiniswap0",
                    "provider_failure_deadline_ms",
                    "2000",
                ),
                ("write_attribute", "infiniswap0", "consumer_id", "consumer-a"),
                ("write_attribute", "infiniswap0", "providers", "provider-a"),
                ("write_attribute", "infiniswap0", "swap_priority", "100"),
                ("write_attribute", "infiniswap0", "state", "activate"),
            ],
        )

    def test_create_requires_root_and_rolls_back_partial_configuration(self):
        path = self.write_consumer()

        unprivileged = FakeSystem()
        unprivileged.root = False
        result, stdout, stderr, _ = self.invoke_create_without_dry_run(
            path, unprivileged
        )
        self.assertEqual(result, 2)
        self.assertEqual(stdout, "")
        self.assertIn("must run as root", stderr)
        self.assertEqual(unprivileged.mutations, [])

        failing = FakeSystem()
        failing.fail_attr = "capacity_bytes"
        result, stdout, stderr, _ = self.invoke_create_without_dry_run(path, failing)
        self.assertEqual(result, 1)
        self.assertIn("injected write failure", stderr)
        self.assertEqual(failing.mutations[-1], ("remove_group", "infiniswap0"))
        self.assertNotIn("infiniswap0", failing.groups)

    def invoke_create_without_dry_run(self, path, system):
        stdout = io.StringIO()
        stderr = io.StringIO()
        result = run(
            ["create", "--config", str(path)],
            system=system,
            stdout=stdout,
            stderr=stderr,
        )
        return result, stdout.getvalue(), stderr.getvalue(), system


class ProviderValidationTest(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.path = Path(self.temporary_directory.name) / "provider.json"
        self.provider = {
            "schema_version": 1,
            "identity": {"provider_id": "provider-a"},
            "listen": {"address": "::", "port": 9400},
            "rdma_rail": {
                "device": "mlx5_ib2",
                "port": 1,
                "numa_node": 0,
            },
            "pools": {
                "host_reserve_gib": 8,
                "max_opportunistic_gib": 24,
                "max_committed_gib": 8,
            },
            "consumers": {
                "consumer-a": {
                    "quota": {
                        "max_connections": 1,
                        "max_opportunistic_gib": 16,
                        "max_committed_gib": 4,
                    },
                    "authentication": {
                        "current": {
                            "key_id": "key-current",
                            "psk_file": "/etc/infiniswap/keys/consumer-a.psk",
                        },
                    },
                },
            },
        }

    def tearDown(self):
        self.temporary_directory.cleanup()

    def validate(self, document, system=None):
        self.path.write_text(json.dumps(document), encoding="utf-8")
        stdout = io.StringIO()
        stderr = io.StringIO()
        system = system or FakeSystem()
        result = run(
            ["validate", "provider", "--config", str(self.path)],
            system=system,
            stdout=stdout,
            stderr=stderr,
        )
        return result, stdout.getvalue(), stderr.getvalue(), system

    def test_provider_validation_accepts_explicit_pools_quotas_psk_and_rail(self):
        result, stdout, stderr, system = self.validate(self.provider)

        self.assertEqual(result, 0)
        self.assertEqual(stderr, "")
        self.assertIn("Provider configuration is valid: provider-a", stdout)
        self.assertEqual(system.mutations, [])

    def test_provider_validation_rejects_unsafe_capacity_quota_psk_and_rail(self):
        cases = []

        oversized_pools = json.loads(json.dumps(self.provider))
        oversized_pools["pools"]["max_opportunistic_gib"] = 121
        cases.append((oversized_pools, None, "pool maxima may total at most 128 GiB"))

        oversized_quota = json.loads(json.dumps(self.provider))
        oversized_quota["consumers"]["consumer-a"]["quota"]["max_opportunistic_gib"] = (
            25
        )
        cases.append(
            (
                oversized_quota,
                None,
                "quota.max_opportunistic_gib exceeds the Opportunistic Pool",
            )
        )

        insecure_system = FakeSystem()
        insecure_system.file_stats["/etc/infiniswap/keys/consumer-a.psk"] = (
            SimpleNamespace(st_mode=stat.S_IFREG | 0o640, st_uid=0)
        )
        cases.append(
            (self.provider, insecure_system, "must be owned by root with mode 0600")
        )

        wrong_rail = json.loads(json.dumps(self.provider))
        wrong_rail["rdma_rail"]["numa_node"] = 1
        cases.append(
            (wrong_rail, None, "rdma_rail.numa_node does not match mlx5_ib2 port 1")
        )

        for document, system, message in cases:
            with self.subTest(message=message):
                result, stdout, stderr, used_system = self.validate(document, system)
                self.assertEqual(result, 2)
                self.assertEqual(stdout, "")
                self.assertIn(message, stderr)
                self.assertEqual(used_system.mutations, [])


class LifecycleCommandTest(unittest.TestCase):
    def setUp(self):
        self.system = FakeSystem()
        self.system.groups.add("infiniswap0")
        self.system.attributes["infiniswap0"] = {
            "state": "active",
            "mode": "backed",
            "acknowledgement_policy": "strict",
            "backing_store": "/dev/vdb",
            "capacity_bytes": str(8 * GIB),
            "provider_failure_deadline_ms": "2000",
            "consumer_id": "consumer-a",
            "providers": "provider-a",
            "swap_priority": "100",
            "connection_state": "not-connected",
            "remote_capacity_bytes": "0",
            "last_error": "0",
        }

    def invoke(self, *arguments):
        stdout = io.StringIO()
        stderr = io.StringIO()
        result = run(list(arguments), system=self.system, stdout=stdout, stderr=stderr)
        return result, stdout.getvalue(), stderr.getvalue()

    def test_lifecycle_commands_only_mutate_the_named_device(self):
        result, stdout, stderr = self.invoke("format", "infiniswap0", "--yes")
        self.assertEqual((result, stderr), (0, ""))
        self.assertIn("Preflight format infiniswap0", stdout)

        result, _, stderr = self.invoke("enable", "infiniswap0", "--priority", "100")
        self.assertEqual((result, stderr), (0, ""))
        self.assertEqual(self.system.swap["/dev/infiniswap0"], 100)

        result, _, stderr = self.invoke("disable", "infiniswap0")
        self.assertEqual((result, stderr), (0, ""))
        self.assertNotIn("/dev/infiniswap0", self.system.swap)
        self.assertEqual(self.system.swap, {"/dev/nvme0n1p2": -2})

        result, _, stderr = self.invoke("drain", "infiniswap0")
        self.assertEqual((result, stderr), (0, ""))
        self.assertEqual(self.system.attributes["infiniswap0"]["state"], "drain")
        self.system.attributes["infiniswap0"]["state"] = "drained"

        result, _, stderr = self.invoke("destroy", "infiniswap0")
        self.assertEqual((result, stderr), (0, ""))
        self.assertNotIn("infiniswap0", self.system.groups)
        self.assertEqual(self.system.swap, {"/dev/nvme0n1p2": -2})
        self.assertEqual(
            self.system.mutations,
            [
                ("run_host_command", ("mkswap", "/dev/infiniswap0")),
                (
                    "run_host_command",
                    ("swapon", "--priority", "100", "/dev/infiniswap0"),
                ),
                ("run_host_command", ("swapoff", "/dev/infiniswap0")),
                ("write_attribute", "infiniswap0", "state", "drain"),
                ("write_attribute", "infiniswap0", "state", "stop"),
                ("remove_group", "infiniswap0"),
            ],
        )

    def test_drain_rejects_enabled_swap_without_disabling_any_swap(self):
        self.system.swap["/dev/infiniswap0"] = 100

        result, stdout, stderr = self.invoke("drain", "infiniswap0")

        self.assertEqual(result, 2)
        self.assertEqual(stdout, "")
        self.assertIn("disable infiniswap0 before draining", stderr)
        self.assertEqual(self.system.mutations, [])
        self.assertEqual(
            self.system.swap,
            {
                "/dev/nvme0n1p2": -2,
                "/dev/infiniswap0": 100,
            },
        )

    def test_enable_rejects_priority_that_differs_from_validated_config(self):
        self.system.swap_signatures.add("/dev/infiniswap0")

        result, stdout, stderr = self.invoke(
            "enable", "infiniswap0", "--priority", "101"
        )

        self.assertEqual(result, 2)
        self.assertEqual(stdout, "")
        self.assertIn("configured swap priority 100", stderr)
        self.assertEqual(self.system.mutations, [])
        self.assertEqual(self.system.swap, {"/dev/nvme0n1p2": -2})

    def test_status_has_stable_sanitized_json_and_human_output(self):
        self.system.swap["/dev/infiniswap0"] = 100
        stdout = io.StringIO()
        stderr = io.StringIO()

        result = run(
            ["status", "infiniswap0", "--json"],
            system=self.system,
            stdout=stdout,
            stderr=stderr,
        )

        snapshot = (Path(__file__).parent / "snapshots" / "status-v1.json").read_text(
            encoding="utf-8"
        )
        self.assertEqual((result, stderr.getvalue()), (0, ""))
        self.assertEqual(stdout.getvalue(), snapshot)
        self.assertNotIn("psk", stdout.getvalue().lower())
        self.assertNotIn("key-current", stdout.getvalue())

        result, human, error = self.invoke("status", "infiniswap0")
        self.assertEqual((result, error), (0, ""))
        self.assertIn("infiniswap0: active (backed, strict)", human)
        self.assertIn("connection: not-connected (0/1 Providers healthy)", human)
        self.assertIn("swap: enabled at priority 100", human)
        self.assertIn("last error: none", human)

    def test_every_host_affecting_command_has_a_non_mutating_dry_run(self):
        cases = [
            ("format", "infiniswap0", "--dry-run"),
            ("enable", "infiniswap0", "--priority", "100", "--dry-run"),
            ("disable", "infiniswap0", "--dry-run"),
            ("drain", "infiniswap0", "--dry-run"),
            ("destroy", "infiniswap0", "--dry-run"),
        ]
        self.system.swap_signatures.add("/dev/infiniswap0")
        for arguments in cases:
            with self.subTest(command=arguments[0]):
                if arguments[0] == "enable":
                    self.system.swap.pop("/dev/infiniswap0", None)
                elif arguments[0] == "disable":
                    self.system.swap["/dev/infiniswap0"] = 100
                elif arguments[0] in ("drain", "destroy"):
                    self.system.swap.pop("/dev/infiniswap0", None)
                if arguments[0] == "destroy":
                    self.system.attributes["infiniswap0"]["state"] = "drained"
                before = list(self.system.mutations)
                result, stdout, stderr = self.invoke(*arguments)
                self.assertEqual((result, stderr), (0, ""))
                self.assertIn("Dry run: no changes made", stdout)
                self.assertEqual(self.system.mutations, before)


if __name__ == "__main__":
    unittest.main()
