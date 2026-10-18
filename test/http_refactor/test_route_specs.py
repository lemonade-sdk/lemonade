"""Pure in-memory spec and response validation tests."""

from pathlib import Path
import tempfile
import unittest

from route_specs import (
    check_json,
    expanded_paths,
    sample_path,
    validate_docs,
    validate_routes,
)


def fixture():
    return {
        "id": "lemonade.unit_test",
        "methods": ["POST"],
        "paths": ["things/{id}", "other/{id}"],
        "prefixes": "Quad",
        "request_format": "Json",
        "error_format": "OpenAi",
        "args": [
            {
                "name": "id",
                "in": "Path",
                "required": True,
                "forwarded": False,
                "aliases": [],
                "schema": {"type": "integer"},
            },
            {
                "name": "title",
                "in": "JsonBody",
                "required": True,
                "forwarded": False,
                "aliases": [],
                "schema": {"type": "string"},
            },
        ],
        "responses": [
            {
                "format": "Json",
                "schema": {
                    "type": "object",
                    "required": ["ok"],
                    "properties": {"ok": {"type": "boolean"}},
                },
            }
        ],
        "examples": [{"format": "Json", "name": "", "setup": []}],
    }


class RouteSpecTest(unittest.TestCase):
    def test_valid_manifest(self):
        self.assertEqual(validate_routes([fixture()]), [])

    def test_missing_route_id(self):
        route = fixture()
        route["id"] = "broken"
        self.assertTrue(validate_routes([route]))

    def test_duplicate_manifest_id(self):
        self.assertIn(
            "duplicate route id", str(validate_routes([fixture(), fixture()]))
        )

    def test_unsafe_method_not_supported(self):
        route = fixture()
        route["methods"] = ["PATCH"]
        self.assertTrue(validate_routes([route]))

    def test_missing_path_argument(self):
        route = fixture()
        route["args"] = route["args"][1:]
        self.assertIn("path parameters", str(validate_routes([route])))

    def test_alias_path_cannot_reorder_parameters(self):
        route = fixture()
        route["paths"][1] = "other/{title}"
        self.assertIn("positional path", str(validate_routes([route])))

    def test_schema_required_fields_must_exist_in_properties(self):
        route = fixture()
        route["responses"][0]["schema"]["required"] = ["missing"]
        self.assertIn("missing", str(validate_routes([route])))

    def test_examples_cannot_reference_missing_route(self):
        route = fixture()
        route["examples"][0]["setup"] = [{"route": "no.such_route"}]
        self.assertIn("no.such_route", str(validate_routes([route])))

    def test_prefix_expansion(self):
        self.assertEqual(len(expanded_paths(fixture())), 8)
        route = fixture()
        route["prefixes"] = "Internal"
        self.assertIn("/internal/things/{id}", expanded_paths(route))

    def test_sample_path_fills_numeric_capture(self):
        self.assertEqual(sample_path(fixture(), "/v1/things/{id}"), "/v1/things/1")

    def test_manifest_ensures_empty_array_does_not_pass(self):
        self.assertTrue(validate_routes([]))

    def test_response_const_and_types_checked(self):
        schema = {
            "type": "object",
            "required": ["count"],
            "properties": {"count": {"type": "integer"}},
        }
        self.assertEqual(check_json(schema, {"count": 3}), [])
        self.assertTrue(check_json(schema, {"count": "3"}))
        self.assertTrue(check_json(schema, {"count": True}))
        self.assertTrue(check_json(schema, {}))

    def test_nested_array_schema(self):
        schema = {
            "type": "array",
            "items": {
                "type": "object",
                "required": ["id"],
                "properties": {"id": {"type": "string"}},
            },
        }
        self.assertEqual(check_json(schema, [{"id": "ok"}]), [])
        self.assertTrue(check_json(schema, [{"id": 4}]))

    def test_duplicate_http_method_path_between_distinct_routes(self):
        first = fixture()
        second = fixture()
        second["id"] = "lemonade.second"
        self.assertIn("duplicates", str(validate_routes([first, second])))

    def test_path_alias_with_different_parameter_order_rejected(self):
        route = fixture()
        route["paths"] = ["items/{id}/{slug}", "items/{slug}/{id}"]
        route["args"].append(
            {
                "name": "slug",
                "in": "Path",
                "required": True,
                "forwarded": False,
                "aliases": [],
                "schema": {"type": "string"},
            }
        )
        self.assertIn("positional path", str(validate_routes([route])))

    def test_unknown_response_schema_type_is_rejected(self):
        route = fixture()
        route["responses"][0]["schema"]["properties"]["ok"]["type"] = "imaginary"
        self.assertIn("invalid JSON schema type", str(validate_routes([route])))

    def test_response_enum_is_checked(self):
        schema = {
            "type": "object",
            "properties": {"status": {"enum": ["ok", "failed"]}},
        }
        self.assertEqual(check_json(schema, {"status": "ok"}), [])
        self.assertIn("enum", str(check_json(schema, {"status": "bad"})))

    def test_sample_path_fills_string_parameter(self):
        route = fixture()
        self.assertEqual(sample_path(route, "/v1/items/{id}"), "/v1/items/1")
        route["args"][0]["schema"]["type"] = "string"
        self.assertEqual(sample_path(route, "/v1/items/{id}"), "/v1/items/test")

    def test_docs_require_generated_marker(self):
        with tempfile.TemporaryDirectory() as tmp:
            docs = Path(tmp) / "docs" / "api"
            docs.mkdir(parents=True)
            (docs / "lemonade.md").write_text(
                "<!-- BEGIN GENERATED: lemonade.unit_test -->", encoding="utf-8"
            )
            self.assertEqual(validate_docs([fixture()], Path(tmp)), [])
            (docs / "lemonade.md").write_text("# Missing", encoding="utf-8")
            self.assertTrue(validate_docs([fixture()], Path(tmp)))


if __name__ == "__main__":
    unittest.main()
