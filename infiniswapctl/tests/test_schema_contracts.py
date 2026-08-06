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
        paths.append(Path(__file__).parent / "snapshots" / "status-v1.json")
        for path in paths:
            with self.subTest(path=path.name):
                with path.open("r", encoding="utf-8") as stream:
                    self.assertIsInstance(json.load(stream), dict)

    @unittest.skipIf(jsonschema is None, "python3-jsonschema is not installed")
    def test_version_one_examples_and_status_snapshot_match_their_schemas(self):
        pairs = (
            ("consumer.schema.json", "consumer.example.json"),
            ("provider.schema.json", "provider.example.json"),
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

        status_schema = json.loads(
            (CONFIG / "status.schema.json").read_text(encoding="utf-8")
        )
        status = json.loads(
            (Path(__file__).parent / "snapshots" / "status-v1.json").read_text(
                encoding="utf-8"
            )
        )
        jsonschema.Draft7Validator.check_schema(status_schema)
        jsonschema.validate(status, status_schema)


if __name__ == "__main__":
    unittest.main()
