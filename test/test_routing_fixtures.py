"""Validate the committed routing fixtures against the frozen JSON Schemas.

Acceptance: the lean L0a-L3 ``collection.router`` example fixtures validate
against the route-policy schema, and the decision example validates against the
decision schema.

Run:  python test/test_routing_fixtures.py
or:   python -m unittest test.test_routing_fixtures
"""

import glob
import json
import os
import unittest

import jsonschema

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA_DIR = os.path.join(REPO_ROOT, "src", "cpp", "resources", "schemas")
FIXTURE_DIR = os.path.join(REPO_ROOT, "test", "cpp", "fixtures", "routing")
CORPUS_DIR = os.path.join(REPO_ROOT, "test", "conformance", "routing")

ROUTE_POLICY_FIXTURES = [
    "l0a_llm_router.json",
    "l1_keywords.json",
    "l1_metadata.json",
    "l2_semantic.json",
    "l3_classifier.json",
]


def _load(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


class RoutingFixtureSchemaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.route_policy_schema = _load(
            os.path.join(SCHEMA_DIR, "route_policy.schema.json")
        )
        cls.decision_schema = _load(os.path.join(SCHEMA_DIR, "decision.schema.json"))
        cls.request_schema = _load(os.path.join(SCHEMA_DIR, "request.schema.json"))
        # Fail loudly if a schema is itself malformed.
        jsonschema.Draft202012Validator.check_schema(cls.route_policy_schema)
        jsonschema.Draft202012Validator.check_schema(cls.decision_schema)
        jsonschema.Draft202012Validator.check_schema(cls.request_schema)

    def test_route_policy_fixtures_validate(self):
        validator = jsonschema.Draft202012Validator(self.route_policy_schema)
        for name in ROUTE_POLICY_FIXTURES:
            with self.subTest(fixture=name):
                doc = _load(os.path.join(FIXTURE_DIR, name))
                errors = sorted(validator.iter_errors(doc), key=lambda e: e.path)
                self.assertEqual(
                    errors,
                    [],
                    msg="\n".join(f"{list(e.path)}: {e.message}" for e in errors),
                )

    def test_decision_example_validates(self):
        doc = _load(os.path.join(FIXTURE_DIR, "decision_example.json"))
        jsonschema.validate(doc, self.decision_schema)

    def test_recorded_corpus_decisions_validate(self):
        """Every decision the conformance corpus records must satisfy the schema.

        The corpus is the one place that exercises the serializer's optional
        trace keys (`label`, `rationale`, `error`, `truncated`). `trace_entry`
        is `additionalProperties: false`, so a key the serializer learns to emit
        without being declared in the schema makes every such response invalid
        -- and the single-fixture check above would not notice.
        """
        validator = jsonschema.Draft202012Validator(self.decision_schema)
        cases = glob.glob(os.path.join(CORPUS_DIR, "**", "cases.jsonl"), recursive=True)
        self.assertTrue(cases, "no conformance cases found")
        for path in cases:
            rel = os.path.relpath(path, CORPUS_DIR)
            with open(path, "r", encoding="utf-8") as handle:
                for number, line in enumerate(handle, start=1):
                    if not line.strip():
                        continue
                    row = json.loads(line)
                    decision = row.get("decision")
                    if decision is None:
                        continue
                    with self.subTest(corpus=rel, case=row.get("case_name", number)):
                        errors = sorted(
                            validator.iter_errors(decision), key=lambda e: e.path
                        )
                        self.assertEqual(
                            errors,
                            [],
                            msg="\n".join(
                                f"{list(e.path)}: {e.message}" for e in errors
                            ),
                        )

    def test_request_example_validates(self):
        doc = _load(os.path.join(FIXTURE_DIR, "request_example.json"))
        jsonschema.validate(doc, self.request_schema)

    def test_request_rejects_non_string_metadata(self):
        """metadata values must be strings (list values are comma-encoded)."""
        validator = jsonschema.Draft202012Validator(self.request_schema)
        bad = {"metadata": {"task_class": ["payment", "checkout"]}}
        self.assertTrue(list(validator.iter_errors(bad)))

    def test_classifier_type_specific_requirements(self):
        """Conditional `required` by classifier type: e.g. semantic_similarity
        needs reference_phrases, llm needs prompt."""
        validator = jsonschema.Draft202012Validator(self.route_policy_schema)
        base = {
            "version": "1",
            "recipe": "collection.router",
            "routing": {
                "candidates": ["a"],
                "default_model": "a",
                "classifiers": [
                    {"id": "x", "type": "semantic_similarity", "model": "m"}
                ],
                "rules": [{"id": "r", "match": {"classifier": "x"}, "route_to": "a"}],
            },
        }
        # Missing reference_phrases for semantic_similarity -> invalid.
        self.assertTrue(list(validator.iter_errors(base)))

    def test_on_truncation_only_on_model_backed_types(self):
        """The parser refuses on_truncation outside classifier and zero_shot;
        the schema must refuse the same documents."""
        validator = jsonschema.Draft202012Validator(self.route_policy_schema)
        classifiers = {
            "classifier": {"model": "m"},
            "zero_shot": {"model": "m", "labels": ["x"]},
            "llm": {"model": "m", "prompt": "p", "labels": ["x"]},
            "semantic_similarity": {"model": "m", "reference_phrases": {"x": ["a"]}},
        }
        for type_, fields in classifiers.items():
            policy = {
                "version": "1",
                "recipe": "collection.router",
                "routing": {
                    "candidates": ["a"],
                    "default_model": "a",
                    "classifiers": [
                        {"id": "c", "type": type_, "on_truncation": "fail", **fields}
                    ],
                    "rules": [
                        {"id": "r", "match": {"classifier": "c"}, "route_to": "a"}
                    ],
                },
            }
            errors = list(validator.iter_errors(policy))
            with self.subTest(type=type_):
                if type_ in ("classifier", "zero_shot"):
                    self.assertFalse(errors)
                else:
                    self.assertTrue(errors)

    def test_locked_structural_invariants(self):
        """Cross-field invariants the JSON Schema cannot express: default_model
        and every rule.route_to must be a candidate; classifier condition refs
        must resolve."""
        for name in ROUTE_POLICY_FIXTURES:
            with self.subTest(fixture=name):
                routing = _load(os.path.join(FIXTURE_DIR, name))["routing"]
                candidates = set(routing["candidates"])
                self.assertIn(routing["default_model"], candidates)

                classifier_ids = {c["id"] for c in routing.get("classifiers", [])}
                for rule in routing.get("rules", []):
                    self.assertIn(rule["route_to"], candidates)
                    self._assert_refs_resolve(rule["match"], classifier_ids)

    def _assert_refs_resolve(self, expr, classifier_ids):
        if not isinstance(expr, dict):
            return
        if "classifier" in expr:
            self.assertIn(expr["classifier"], classifier_ids)
        for op in ("any", "all"):
            for child in expr.get(op, []):
                self._assert_refs_resolve(child, classifier_ids)
        if "not" in expr:
            self._assert_refs_resolve(expr["not"], classifier_ids)


if __name__ == "__main__":
    unittest.main(verbosity=2)
