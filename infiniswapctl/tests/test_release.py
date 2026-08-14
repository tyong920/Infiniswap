import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from infiniswapctl.config import GIB
from infiniswapctl.release import (
    ReleaseError,
    assess_drain_capacity,
    migrate_config_document,
    verify_kernel_abi_evidence,
)


class ConfigMigrationTest(unittest.TestCase):
    def test_consumer_v1_migrates_to_current_defaults_without_changing_semantics(self):
        source = {
            "schema_version": 1,
            "identity": {"consumer_id": "consumer-a"},
            "device": {
                "name": "infiniswap0",
                "mode": "backed",
                "backing_store": "/dev/vdb",
            },
        }

        migrated = migrate_config_document("consumer", source)

        self.assertEqual(migrated["schema_version"], 3)
        self.assertEqual(
            migrated["identity"],
            {"consumer_id": "consumer-a", "remote_only_eligible": False},
        )
        self.assertEqual(
            migrated["device"]["hot_range"],
            {"mapping_threshold": 8, "read_weight": 1, "write_weight": 4},
        )
        self.assertEqual(migrated["device"]["backing_store"], "/dev/vdb")
        self.assertNotIn("hot_range", source["device"])

    def test_v1_provider_contracts_migrate_to_current_schema(self):
        for kind in ("provider", "provider-directory"):
            with self.subTest(kind=kind):
                source = {"schema_version": 1, "marker": kind}
                migrated = migrate_config_document(kind, source)
                self.assertEqual(migrated, {"schema_version": 2, "marker": kind})
                self.assertEqual(source["schema_version"], 1)

    def test_migration_rejects_current_or_unknown_versions(self):
        cases = (("consumer", 3), ("provider", 2), ("provider-directory", 99))
        for kind, version in cases:
            with self.subTest(kind=kind, version=version):
                with self.assertRaises(ReleaseError):
                    migrate_config_document(kind, {"schema_version": version})


class DrainCapacityTest(unittest.TestCase):
    def test_reserve_is_kept_before_alternate_capacity_is_counted(self):
        assessment = assess_drain_capacity(
            target_swap_used_bytes=3 * GIB,
            memory_available_bytes=3 * GIB,
            alternate_swap_free_bytes=GIB,
            reserve_bytes=GIB,
        )

        self.assertTrue(assessment.sufficient)
        self.assertEqual(assessment.absorbable_bytes, 3 * GIB)

    def test_insufficient_capacity_is_rejected(self):
        assessment = assess_drain_capacity(
            target_swap_used_bytes=3 * GIB,
            memory_available_bytes=2 * GIB,
            alternate_swap_free_bytes=GIB,
            reserve_bytes=GIB,
        )

        self.assertFalse(assessment.sufficient)
        self.assertEqual(assessment.shortfall_bytes, GIB)


class KernelAbiEvidenceTest(unittest.TestCase):
    def _write_report(self, root, name, document):
        path = root / (name + ".json")
        path.write_text(json.dumps(document) + "\n", encoding="ascii")
        return {
            "path": str(path),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "passed": True,
        }

    def test_supported_tuple_requires_matching_compile_vm_and_canary_evidence(self):
        commit = "a" * 40
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dkms = self._write_report(
                root,
                "dkms",
                {
                    "schema_version": 1,
                    "kind": "infiniswap.dkms-report",
                    "status": "passed",
                    "source_commit": commit,
                    "ubuntu_release": "24.04",
                    "kernel_release": "6.8.0-51-generic",
                    "rdma_stack": "inbox",
                },
            )
            vm = self._write_report(
                root,
                "vm",
                {
                    "schema_version": 1,
                    "source_commit": commit,
                    "status": "passed",
                    "profile": {"certifiable": True},
                    "matrix": [
                        {
                            "ubuntu": "24.04",
                            "kernel": "6.8",
                            "kernel_release": "6.8.0-51-generic",
                            "rdma_stack": "inbox",
                            "status": "passed",
                        }
                    ],
                    "safety": {"status": "passed"},
                    "cleanup": {"status": "passed"},
                },
            )
            vm["certifiable"] = True
            canary = self._write_report(
                root,
                "canary",
                {
                    "schema_version": 1,
                    "kind": "infiniswap.canary-report",
                    "status": "passed",
                    "source_commit": commit,
                    "ubuntu_release": "24.04",
                    "kernel_release": "6.8.0-51-generic",
                    "rdma_stack": "inbox",
                    "duration_seconds": 3600,
                },
            )
            canary["duration_seconds"] = 3600
            evidence = {
                "schema_version": 1,
                "commit": commit,
                "ubuntu_release": "24.04",
                "kernel_release": "6.8.0-51-generic",
                "rdma_stack": "inbox",
                "dkms": dkms,
                "vm": vm,
                "canary": canary,
            }

            verified = verify_kernel_abi_evidence(
                evidence, expected_commit=commit
            )

            vm_path = Path(vm["path"])
            vm_document = json.loads(vm_path.read_text(encoding="ascii"))
            vm_document["matrix"][0]["kernel_release"] = "6.8.0-50-generic"
            vm_path.write_text(json.dumps(vm_document) + "\n", encoding="ascii")
            vm["sha256"] = hashlib.sha256(vm_path.read_bytes()).hexdigest()
            with self.assertRaises(ReleaseError):
                verify_kernel_abi_evidence(evidence, expected_commit=commit)

        self.assertEqual(verified.kernel_release, "6.8.0-51-generic")

    def test_gate_rejects_hashed_but_unrelated_report_content(self):
        commit = "b" * 40
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reports = {
                name: self._write_report(root, name, {"passed": True})
                for name in ("dkms", "vm", "canary")
            }
            reports["vm"]["certifiable"] = True
            reports["canary"]["duration_seconds"] = 60
            evidence = {
                "schema_version": 1,
                "commit": commit,
                "ubuntu_release": "24.04",
                "kernel_release": "6.8.0-51-generic",
                "rdma_stack": "inbox",
                **reports,
            }
            with self.assertRaises(ReleaseError):
                verify_kernel_abi_evidence(evidence, expected_commit=commit)

    def test_gate_rejects_missing_canary_or_unvalidated_tuple(self):
        evidence = {
            "schema_version": 1,
            "commit": "b" * 40,
            "ubuntu_release": "24.04",
            "kernel_release": "6.9.0-generic",
            "rdma_stack": "inbox",
        }
        with self.assertRaises(ReleaseError):
            verify_kernel_abi_evidence(evidence, expected_commit="b" * 40)


if __name__ == "__main__":
    unittest.main()
