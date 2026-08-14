import re
import unittest
from pathlib import Path

REPOSITORY = Path(__file__).parents[2]
RULES = REPOSITORY / "monitoring" / "infiniswap-alerts.yml"


class ObservabilityContractTest(unittest.TestCase):
    def test_baseline_alerts_are_bounded_sanitized_and_have_runbooks(self):
        document = RULES.read_text(encoding="ascii")
        alerts = set(re.findall(r"^\s*- alert: ([A-Za-z0-9]+)$", document, re.M))
        self.assertEqual(
            alerts,
            {
                "InfiniswapProviderDisconnected",
                "InfiniswapProviderDeadlineExpired",
                "InfiniswapProviderDegraded",
                "InfiniswapBackingDegraded",
                "InfiniswapRemoteLost",
                "InfiniswapPoolPressure",
                "InfiniswapAdmissionRejected",
                "InfiniswapAuthenticationFailure",
                "InfiniswapIOErrors",
                "InfiniswapHungRequests",
            },
        )
        runbooks = re.findall(r"runbook: (docs/runbooks/[a-z0-9-]+\.md)", document)
        self.assertEqual(len(runbooks), len(alerts))
        operation_runbooks = (
            "docs/runbooks/normal-start-stop.md",
            "docs/runbooks/capacity-change.md",
            "docs/runbooks/psk-rotation-revocation.md",
            "docs/runbooks/provider-rolling-upgrade.md",
            "docs/runbooks/consumer-drain.md",
            "docs/runbooks/rollback.md",
        )
        for runbook in tuple(runbooks) + operation_runbooks:
            with self.subTest(runbook=runbook):
                text = (REPOSITORY / runbook).read_text(encoding="ascii")
                for heading in (
                    "## Preflight",
                    "## Expected output",
                    "## Abort conditions",
                    "## Recovery and rollback",
                ):
                    self.assertIn(heading, text)
        lowered = document.lower()
        for forbidden in (
            "psk",
            "key_id",
            "request_id",
            "chunk_id",
            "sector",
            "error_message",
        ):
            self.assertNotIn(forbidden, lowered)

    def test_openmetrics_v1_contracts_end_cleanly_and_bound_labels(self):
        paths = (
            REPOSITORY
            / "infiniswapctl"
            / "tests"
            / "snapshots"
            / "consumer-metrics-v1.txt",
            REPOSITORY
            / "infiniswap_daemon"
            / "tests"
            / "provider-metrics-v1.txt",
        )
        for path in paths:
            with self.subTest(path=path.name):
                metrics = path.read_text(encoding="ascii")
                self.assertTrue(
                    metrics.startswith("# infiniswap_metrics_schema_version 1\n")
                )
                self.assertTrue(metrics.endswith("# EOF\n"))
                type_families = re.findall(r"^# TYPE ([a-z0-9_]+) ", metrics, re.M)
                self.assertEqual(len(type_families), len(set(type_families)))
                label_names = set(
                    re.findall(r"(?:\{|,)([a-z_]+)=\"", metrics)
                )
                self.assertLessEqual(
                    label_names,
                    {
                        "device",
                        "mode",
                        "policy",
                        "state",
                        "provider",
                        "pool",
                        "consumer",
                    },
                )
                self.assertNotIn("psk", metrics.lower())
                self.assertNotIn("key_id", metrics.lower())


if __name__ == "__main__":
    unittest.main()
