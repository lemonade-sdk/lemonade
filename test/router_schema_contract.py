"""
Contract tests for Lemonade Router JSON Schemas.

These tests validate the schema-gate fixtures for:
  - collection.router model definitions (#2375)
  - route request/response extensions (#2376)

They do not exercise runtime parser cross-reference checks such as
route_to-in-candidates or candidate-in-components; those belong to the parser
implementation.
"""

import json
import unittest
from pathlib import Path

from jsonschema import Draft202012Validator, ValidationError


REPO_ROOT = Path(__file__).resolve().parents[1]
SCHEMA_DIR = REPO_ROOT / "src" / "cpp" / "resources" / "schemas"
FIXTURE_DIR = REPO_ROOT / "test" / "fixtures" / "router"

COLLECTION_ROUTER_SCHEMA = SCHEMA_DIR / "collection-router.schema.json"
ROUTE_DECISION_SCHEMA = SCHEMA_DIR / "route-decision.schema.json"

COLLECTION_ROUTER_VALID = [
    "valid/l0a_llm_router.json",
    "valid/l1_deterministic_rules.json",
    "valid/l2_semantic_similarity.json",
    "valid/l3_classifier.json",
]

COLLECTION_ROUTER_INVALID = [
    "invalid/router_missing_candidates.json",
    "invalid/router_missing_rules_and_router.json",
    "invalid/router_empty_match.json",
    "invalid/router_mixed_logical_leaf.json",
    "invalid/router_invalid_classifier_type.json",
]

ROUTE_DECISION_VALID = [
    "valid/x_lemonade_route_without_trace.json",
    "valid/x_lemonade_route_with_trace.json",
    "valid/route_request_extensions.json",
]

ROUTE_DECISION_INVALID = [
    "invalid/route_metadata_nested_value.json",
]


def load_json(path: Path):
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


class RouterSchemaContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.collection_router_schema = load_json(COLLECTION_ROUTER_SCHEMA)
        cls.route_decision_schema = load_json(ROUTE_DECISION_SCHEMA)

        Draft202012Validator.check_schema(cls.collection_router_schema)
        Draft202012Validator.check_schema(cls.route_decision_schema)

        cls.collection_router_validator = Draft202012Validator(cls.collection_router_schema)
        cls.route_decision_validator = Draft202012Validator(cls.route_decision_schema)

    def assert_valid(self, validator: Draft202012Validator, fixture: str):
        path = FIXTURE_DIR / fixture
        instance = load_json(path)
        errors = sorted(validator.iter_errors(instance), key=lambda e: list(e.path))
        if errors:
            detail = "\n".join(
                f"{'/'.join(str(p) for p in error.path) or '<root>'}: {error.message}"
                for error in errors
            )
            self.fail(f"{fixture} should be valid, got:\n{detail}")

    def assert_invalid(self, validator: Draft202012Validator, fixture: str):
        path = FIXTURE_DIR / fixture
        instance = load_json(path)
        with self.assertRaises(ValidationError, msg=f"{fixture} should be invalid"):
            validator.validate(instance)

    def test_collection_router_valid_fixtures(self):
        for fixture in COLLECTION_ROUTER_VALID:
            with self.subTest(fixture=fixture):
                self.assert_valid(self.collection_router_validator, fixture)

    def test_collection_router_invalid_fixtures(self):
        for fixture in COLLECTION_ROUTER_INVALID:
            with self.subTest(fixture=fixture):
                self.assert_invalid(self.collection_router_validator, fixture)

    def test_route_decision_valid_fixtures(self):
        for fixture in ROUTE_DECISION_VALID:
            with self.subTest(fixture=fixture):
                self.assert_valid(self.route_decision_validator, fixture)

    def test_route_decision_invalid_fixtures(self):
        for fixture in ROUTE_DECISION_INVALID:
            with self.subTest(fixture=fixture):
                self.assert_invalid(self.route_decision_validator, fixture)


if __name__ == "__main__":
    unittest.main(verbosity=2)
