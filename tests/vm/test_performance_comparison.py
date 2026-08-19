import io
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

VM_DIR = Path(__file__).resolve().parent
if str(VM_DIR) not in sys.path:
    sys.path.insert(0, str(VM_DIR))

import compare_performance  # noqa: E402


class PerformanceComparisonTest(unittest.TestCase):
    def _write_report(
        self,
        root,
        name,
        *,
        iops=(100.0,) * 5,
        throughput=(409600.0,) * 5,
        p99=(1000.0,) * 5,
        system_cpu=(20.0,) * 5,
        environment=None,
        report_status="passed",
        evidence=None,
        cleanup_status="passed",
        safety_status="passed",
        fio_error=0,
    ):
        report_dir = root / name
        performance_dir = report_dir / "performance"
        performance_dir.mkdir(parents=True)
        environment = environment or {}
        metadata_base = {
            "cloud_image_sha256": "image-sha256",
            "host_identity": "performance-host",
            "ubuntu_release": "24.04",
            "kernel_release": "6.8.0-137-generic",
            "rdma_stack": "inbox",
            "topology": 2,
            "vm_resources": {"vcpus": 6, "memory_gib": 48, "disk_gib": 100},
            "guests": [
                {"name": "consumer", "role": "consumer", "vcpus": 3},
                {"name": "provider-0", "role": "provider", "vcpus": 3},
            ],
            "duration_seconds": 60,
            "fio_arguments": [
                "--name=performance-sample",
                "--runtime=60",
                "--verify=crc32c",
                "--verify_fatal=1",
                "--output=/tmp/fio.json",
            ],
            "fio_verification_arguments": [],
            **environment,
        }
        artifacts = []
        for mode in ("backed", "remote-only"):
            values = [("warmup", 1, 100.0, 409600.0, 1000.0, 20.0)]
            values.extend(
                ("measured", index + 1, *sample)
                for index, sample in enumerate(zip(iops, throughput, p99, system_cpu))
            )
            for phase, index, sample_iops, sample_bw, sample_p99, sample_cpu in values:
                sample = {
                    "schema_version": 1,
                    "kind": "infiniswap.soft-roce-fio-sample",
                    "metadata": {
                        **metadata_base,
                        "source_commit": ("a" if name == "baseline" else "b") * 40,
                        "mode": mode,
                        "phase": phase,
                        "sample_index": index,
                        "fio_arguments": metadata_base["fio_arguments"]
                        + (
                            ["--do_verify=0"]
                            if mode == "remote-only"
                            else ["--do_verify=1"]
                        ),
                        "fio_verification_arguments": (
                            [
                                "--name=performance-sample-verify",
                                "--rw=read",
                                "--verify=crc32c",
                                "--verify_only=1",
                                "--verify_fatal=1",
                                "--output=/tmp/fio-verify.json",
                            ]
                            if mode == "remote-only"
                            else []
                        ),
                    },
                    "fio": {
                        "jobs": [
                            {
                                "error": fio_error,
                                "read": {
                                    "iops": sample_iops,
                                    "bw_bytes": sample_bw,
                                    "clat_ns": {
                                        "percentile": {"99.000000": sample_p99}
                                    },
                                },
                                "write": {
                                    "iops": 0.0,
                                    "bw_bytes": 0.0,
                                    "clat_ns": {"percentile": {"99.000000": 0.0}},
                                },
                                "sys_cpu": sample_cpu,
                            }
                        ]
                    },
                    "verification": (
                        {
                            "jobs": [
                                {
                                    "error": fio_error,
                                    "read": {
                                        "iops": sample_iops,
                                        "bw_bytes": sample_bw,
                                        "clat_ns": {
                                            "percentile": {"99.000000": sample_p99}
                                        },
                                    },
                                    "write": {
                                        "iops": 0.0,
                                        "bw_bytes": 0.0,
                                    },
                                    "sys_cpu": sample_cpu,
                                }
                            ]
                        }
                        if mode == "remote-only"
                        else None
                    ),
                }
                path = performance_dir / ("%s-%s-%d.json" % (mode, phase, index))
                path.write_text(json.dumps(sample) + "\n", encoding="utf-8")
                artifacts.append(str(path))
        clean_evidence = {
            mode: {
                "status": "passed",
                "provider_timeouts_total": 0,
                "late_rdma_completions_total": 0,
                "io_errors_total": 0,
                "io_errors_before_measured": 0,
                "inflight_io": 0,
                "oldest_inflight_ms": 0,
            }
            for mode in ("backed", "remote-only")
        }
        if evidence:
            clean_evidence["backed"].update(evidence)
        report = {
            "schema_version": 1,
            "source_commit": ("a" if name == "baseline" else "b") * 40,
            "status": report_status,
            "profile": {"certifiable": False},
            "certification": {"status": "non-certifiable"},
            "selection": {"scenario": "fio-verification"},
            "safety": {"status": safety_status},
            "cleanup": {"status": cleanup_status},
            "matrix": [
                {
                    "status": report_status,
                    "scenarios": [
                        {"id": "build-deploy", "status": "passed"},
                        {
                            "id": "fio-verification",
                            "status": report_status,
                            "artifacts": artifacts,
                            "metrics": {"evidence": clean_evidence},
                        },
                        {
                            "id": "resource-leak-check",
                            "status": "passed"
                            if cleanup_status == "passed"
                            else "failed",
                            "metrics": {
                                "leaks": [] if cleanup_status == "passed" else ["qemu"]
                            },
                        },
                    ],
                }
            ],
        }
        report_path = report_dir / "report.json"
        report_path.write_text(json.dumps(report) + "\n", encoding="utf-8")
        return report_path

    def _compare(self, baseline_options=None, candidate_options=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline = self._write_report(root, "baseline", **(baseline_options or {}))
            candidate = self._write_report(
                root, "candidate", **(candidate_options or {})
            )
            return compare_performance.compare_reports(baseline, candidate)

    def test_comparable_stable_reports_pass_with_machine_readable_metrics(self):
        result = self._compare(
            candidate_options={
                "iops": (96.0,) * 5,
                "throughput": (390000.0,) * 5,
                "p99": (1090.0,) * 5,
                "system_cpu": (22.0,) * 5,
            }
        )

        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["kind"], "infiniswap.soft-roce-comparison")
        self.assertEqual(set(result["modes"]), {"backed", "remote-only"})
        self.assertEqual(result["thresholds"]["iops_regression_percent"], 5.0)

    def test_mismatched_environment_is_invalid(self):
        result = self._compare(
            candidate_options={"environment": {"host_identity": "other-host"}}
        )

        self.assertEqual(result["status"], "invalid")
        self.assertIn("host_identity", result["reasons"][0])

    def test_each_unstable_baseline_threshold_is_invalid(self):
        cases = {
            "iops": {"iops": (80.0, 100.0, 100.0, 100.0, 120.0)},
            "throughput": {
                "throughput": (320000.0, 409600.0, 409600.0, 409600.0, 499200.0)
            },
            "p99_latency": {"p99": (750.0, 1000.0, 1000.0, 1000.0, 1250.0)},
        }
        for metric, options in cases.items():
            with self.subTest(metric=metric):
                result = self._compare(baseline_options=options)
                self.assertEqual(result["status"], "invalid")
                self.assertTrue(any(metric in reason for reason in result["reasons"]))

    def test_exact_baseline_stability_thresholds_are_valid(self):
        iops_delta = 5.0 * math.sqrt(5.0 / 2.0)
        throughput_delta = 20480.0 * math.sqrt(5.0 / 2.0)
        p99_delta = 100.0 * math.sqrt(5.0 / 2.0)

        result = self._compare(
            baseline_options={
                "iops": (100.0 - iops_delta, 100.0, 100.0, 100.0, 100.0 + iops_delta),
                "throughput": (
                    409600.0 - throughput_delta,
                    409600.0,
                    409600.0,
                    409600.0,
                    409600.0 + throughput_delta,
                ),
                "p99": (
                    1000.0 - p99_delta,
                    1000.0,
                    1000.0,
                    1000.0,
                    1000.0 + p99_delta,
                ),
            }
        )

        self.assertEqual(result["status"], "passed")

    def test_each_performance_regression_threshold_fails(self):
        cases = {
            "iops": {"iops": (94.9,) * 5},
            "throughput": {"throughput": (389000.0,) * 5},
            "p99_latency": {"p99": (1101.0,) * 5},
            "system_cpu": {"system_cpu": (22.1,) * 5},
        }
        for metric, options in cases.items():
            with self.subTest(metric=metric):
                result = self._compare(candidate_options=options)
                self.assertEqual(result["status"], "failed")
                self.assertTrue(any(metric in reason for reason in result["reasons"]))

    def test_system_cpu_increase_requires_reproduction_in_three_samples(self):
        result = self._compare(
            candidate_options={"system_cpu": (22.1, 22.1, 20.0, 20.0, 20.0)}
        )

        self.assertEqual(result["status"], "passed")

    def test_exact_verdict_thresholds_pass(self):
        result = self._compare(
            candidate_options={
                "iops": (95.0,) * 5,
                "throughput": (389120.0,) * 5,
                "p99": (1100.0,) * 5,
                "system_cpu": (22.0,) * 5,
            }
        )

        self.assertEqual(result["status"], "passed")

    def test_cli_persists_machine_readable_verdict_and_exit_status(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline = self._write_report(root, "baseline")
            candidate = self._write_report(root, "candidate", iops=(94.9,) * 5)
            output = root / "comparison.json"
            stdout = io.StringIO()

            status = compare_performance.main(
                [str(baseline), str(candidate), "--output", str(output)],
                stdout=stdout,
            )
            emitted = json.loads(stdout.getvalue())
            persisted = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(status, 1)
        self.assertEqual(emitted, persisted)
        self.assertEqual(emitted["status"], "failed")

    def test_failed_candidate_with_incomplete_samples_still_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline = self._write_report(root, "baseline")
            candidate = self._write_report(root, "candidate", report_status="failed")
            candidate_document = json.loads(candidate.read_text(encoding="utf-8"))
            missing = Path(
                candidate_document["matrix"][0]["scenarios"][1]["artifacts"][0]
            )
            missing.unlink()

            result = compare_performance.compare_reports(baseline, candidate)

        self.assertEqual(result["status"], "failed")
        self.assertTrue(any("missing" in reason for reason in result["reasons"]))

    def test_correctness_and_cleanup_evidence_fails_regardless_of_performance(self):
        cases = {
            "data mismatch": {"report_status": "failed"},
            "timeout": {"evidence": {"provider_timeouts_total": 1}},
            "late completion": {"evidence": {"late_rdma_completions_total": 1}},
            "I/O error counter": {"evidence": {"io_errors_total": 1}},
            "hung I/O": {"evidence": {"inflight_io": 1, "oldest_inflight_ms": 60000}},
            "kernel diagnostics": {"safety_status": "failed"},
            "cleanup leakage": {"cleanup_status": "failed"},
            "fio verification": {"fio_error": 84},
        }
        for reason, options in cases.items():
            with self.subTest(reason=reason):
                result = self._compare(candidate_options=options)
                self.assertEqual(result["status"], "failed")


if __name__ == "__main__":
    unittest.main()
