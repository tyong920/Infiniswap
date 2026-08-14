import json
import unittest
from pathlib import Path

try:
    import jsonschema
except ImportError:
    jsonschema = None


REPOSITORY = Path(__file__).parents[2]
CONFIG = REPOSITORY / "config"


class SchemaContractTest(unittest.TestCase):
    def test_all_schema_and_example_documents_are_valid_json(self):
        paths = list(CONFIG.glob("*.json"))
        snapshot_directory = Path(__file__).parent / "snapshots"
        paths.extend(
            (
                snapshot_directory / "status-v1.json",
                snapshot_directory / "status-v2.json",
                snapshot_directory / "status-v3.json",
                snapshot_directory / "status-v4.json",
                snapshot_directory / "status-v5.json",
                snapshot_directory / "status-v6.json",
                snapshot_directory / "provider-status-v1.json",
                REPOSITORY
                / "infiniswap_daemon"
                / "tests"
                / "provider-status-v1.json",
            )
        )
        for path in paths:
            with self.subTest(path=path.name):
                with path.open("r", encoding="utf-8") as stream:
                    self.assertIsInstance(json.load(stream), dict)

    @unittest.skipIf(jsonschema is None, "python3-jsonschema is not installed")
    def test_versioned_examples_and_status_snapshots_match_their_schemas(self):
        pairs = (
            ("consumer-v1.schema.json", "consumer-v1.example.json"),
            ("consumer-v2.schema.json", "consumer-v2.example.json"),
            ("consumer.schema.json", "consumer.example.json"),
            ("consumer.schema.json", "consumer-remote-only.example.json"),
            ("provider-v1.schema.json", "provider-v1.example.json"),
            ("provider.schema.json", "provider.example.json"),
            (
                "provider-directory-v1.schema.json",
                "provider-directory-v1.example.json",
            ),
            ("provider-directory.schema.json", "provider-directory.example.json"),
        )
        for schema_name, example_name in pairs:
            with self.subTest(schema=schema_name):
                schema = json.loads((CONFIG / schema_name).read_text(encoding="utf-8"))
                example = json.loads(
                    (CONFIG / example_name).read_text(encoding="utf-8")
                )
                jsonschema.Draft7Validator.check_schema(schema)
                jsonschema.validate(example, schema)

        status_contracts = (
            ("status-v1.schema.json", "status-v1.json"),
            ("status-v2.schema.json", "status-v2.json"),
            ("status-v3.schema.json", "status-v3.json"),
            ("status-v4.schema.json", "status-v4.json"),
            ("status-v5.schema.json", "status-v5.json"),
            ("status.schema.json", "status-v6.json"),
            ("provider-status.schema.json", "provider-status-v1.json"),
        )
        for schema_name, snapshot_name in status_contracts:
            with self.subTest(schema=schema_name):
                status_schema = json.loads(
                    (CONFIG / schema_name).read_text(encoding="utf-8")
                )
                status = json.loads(
                    (Path(__file__).parent / "snapshots" / snapshot_name).read_text(
                        encoding="utf-8"
                    )
                )
                jsonschema.Draft7Validator.check_schema(status_schema)
                jsonschema.validate(status, status_schema)

        provider_schema = json.loads(
            (CONFIG / "provider-status.schema.json").read_text(encoding="utf-8")
        )
        for provider_snapshot in (
            Path(__file__).parent / "snapshots" / "provider-status-v1.json",
            REPOSITORY
            / "infiniswap_daemon"
            / "tests"
            / "provider-status-v1.json",
        ):
            with self.subTest(provider_snapshot=provider_snapshot):
                jsonschema.validate(
                    json.loads(provider_snapshot.read_text(encoding="utf-8")),
                    provider_schema,
                )


if __name__ == "__main__":
    unittest.main()
