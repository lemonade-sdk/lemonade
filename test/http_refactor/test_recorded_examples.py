"""Offline drift guards for the refactor's checked-in API recordings."""

from __future__ import annotations

from collections import Counter
import json
from pathlib import Path
import re
import unittest

from harness import ROOT

API_DOCS = ROOT / "docs" / "api"
CACHE = ROOT / "docs" / "tools" / "api_examples_cache.json"
ROUTES = ROOT / "src" / "cpp" / "server" / "routes"
GENERATED = re.compile(r"<!-- (BEGIN|END) GENERATED: ([a-z0-9_.]+) -->")
ROUTE_ID = re.compile(r's\.id\s*=\s*"([a-z0-9_.]+)"')


class RecordedExamplesContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cache = json.loads(CACHE.read_text(encoding="utf-8"))
        cls.routes_with_examples = set()
        cls.route_ids = set()
        for file in ROUTES.rglob("*.cpp"):
            source = file.read_text(encoding="utf-8")
            ids = set(ROUTE_ID.findall(source))
            cls.route_ids.update(ids)
            if "s.examples =" in source:
                cls.routes_with_examples.update(ids)

    def test_all_recorded_entries_are_named_by_sha256(self):
        self.assertGreaterEqual(len(self.cache), 100)
        self.assertTrue(all(re.fullmatch(r"[0-9a-f]{64}", name) for name in self.cache))

    def test_recorded_examples_cover_declared_routes(self):
        cached = {entry["route"] for entry in self.cache.values()}
        self.assertGreaterEqual(len(cached), 75)
        self.assertEqual(self.routes_with_examples - cached, set())
        self.assertEqual(cached - self.route_ids, set())

    def test_recorded_requests_have_valid_transport_shapes(self):
        required = {"method", "path", "query", "json", "form", "files"}
        for key, entry in self.cache.items():
            with self.subTest(key=key[:12], route=entry["route"]):
                request = entry["request"]
                self.assertEqual(set(request), required)
                self.assertIn(request["method"], {"GET", "POST", "DELETE", "PUT"})
                self.assertTrue(request["path"].startswith("/"))
                self.assertIsInstance(request["query"], dict)
                self.assertIsInstance(request["form"], dict)
                self.assertIsInstance(request["files"], dict)

    def test_recordings_are_successful_examples_not_error_snapshots(self):
        for key, entry in self.cache.items():
            with self.subTest(route=entry["route"], key=key[:12]):
                self.assertTrue(200 <= entry["response"]["status"] < 300)

    def test_recorded_json_responses_have_json_bodies(self):
        for key, entry in self.cache.items():
            if entry["format"] != "Json":
                continue
            with self.subTest(route=entry["route"], key=key[:12]):
                self.assertIn("body", entry["response"])
                self.assertIsInstance(entry["response"]["body"], (dict, list))

    def test_recorded_streams_contain_parseable_structural_events(self):
        for key, entry in self.cache.items():
            if entry["format"] not in {"EventStream", "JsonLines"}:
                continue
            with self.subTest(route=entry["route"], key=key[:12]):
                response = entry["response"]
                self.assertGreater(response["count"], 0)
                self.assertIsInstance(response["head"], list)
                self.assertIsInstance(response["tail"], list)
                self.assertTrue(response["head"] or response["tail"])
                for chunk in response["head"] + response["tail"]:
                    if entry["format"] == "JsonLines":
                        self.assertIsInstance(json.loads(chunk), dict)
                    else:
                        self.assertIn("data: ", chunk)

    def test_binary_recordings_have_nonzero_size_and_media_type(self):
        for entry in self.cache.values():
            if entry["format"] not in {"Binary", "BinaryStream"}:
                continue
            with self.subTest(route=entry["route"]):
                self.assertGreater(entry["response"]["bytes"], 0)
                self.assertIn("/", entry["response"]["content_type"])

    def test_recorded_requests_do_not_contain_live_authorization(self):
        for key, entry in self.cache.items():
            with self.subTest(key=key[:12]):
                request = entry["request"]
                self.assertNotIn("headers", request)
                self.assertNotIn("authorization", json.dumps(request).lower())

    def test_all_recorded_routes_appear_in_generated_docs(self):
        by_page = {}
        for entry in self.cache.values():
            route = entry["route"]
            page = route.split(".", 1)[0]
            by_page.setdefault(page, set()).add(route)
        for page, routes in by_page.items():
            text = (API_DOCS / f"{page}.md").read_text(encoding="utf-8")
            for route in routes:
                with self.subTest(page=page, route=route):
                    self.assertIn(f"<!-- BEGIN GENERATED: {route} -->", text)
                    self.assertIn(f"<!-- END GENERATED: {route} -->", text)

    def test_generated_markers_are_balanced_and_unique(self):
        for file in API_DOCS.glob("*.md"):
            text = file.read_text(encoding="utf-8")
            outside_code = []
            fenced = False
            for line in text.splitlines():
                if line.lstrip().startswith("```"):
                    fenced = not fenced
                    continue
                if not fenced:
                    outside_code.append(line)
            markers = GENERATED.findall("\n".join(outside_code))
            begins = Counter(key for action, key in markers if action == "BEGIN")
            ends = Counter(key for action, key in markers if action == "END")
            with self.subTest(page=file.name):
                self.assertEqual(begins, ends)
                self.assertTrue(all(count == 1 for count in begins.values()))

    def test_all_registered_routes_have_generated_sections(self):
        by_page = {}
        for route_id in self.route_ids:
            by_page.setdefault(route_id.split(".", 1)[0], set()).add(route_id)
        self.assertGreaterEqual(len(self.route_ids), 80)
        for page, route_ids in by_page.items():
            content = (API_DOCS / f"{page}.md").read_text(encoding="utf-8")
            for route_id in route_ids:
                with self.subTest(route=route_id):
                    self.assertIn(f"<!-- BEGIN GENERATED: {route_id} -->", content)

    def test_examples_cover_real_openai_ollama_mcp_and_anthropic_clients(self):
        covered = {entry["route"].split(".", 1)[0] for entry in self.cache.values()}
        self.assertTrue({"openai", "ollama", "mcp", "anthropic"} <= covered)

    def test_recorded_ollama_version_is_json(self):
        entries = [e for e in self.cache.values() if e["route"] == "ollama.version"]
        self.assertEqual(len(entries), 1)
        self.assertIsInstance(entries[0]["response"]["body"].get("version"), str)

    def test_mcp_notification_has_empty_response(self):
        entries = [e for e in self.cache.values() if e["route"] == "mcp.rpc"]
        self.assertTrue(
            any(
                e["format"] == "Empty" and e["response"]["status"] == 202
                for e in entries
            )
        )


if __name__ == "__main__":
    unittest.main()
