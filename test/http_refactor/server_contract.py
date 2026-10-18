"""Black-box contracts for PR #3771 merged with GUI3; no inference required."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
import urllib.parse
import uuid

from harness import Client, IsolatedLemond, find_lemond, multipart
from route_specs import (
    QUAD,
    check_json,
    expanded_paths,
    sample_path,
    validate_docs,
    validate_routes,
)

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BINARY: Path | None = None
WITH_GUI3 = False


def route_by_id(routes: list[dict], key: str) -> dict:
    return next(route for route in routes if route["id"] == key)


def error_text(response) -> str:
    try:
        return json.dumps(response.json)
    except ValueError:
        return response.text


class LiveBase:
    api_key = ""
    admin_key = ""
    server_options: dict[str, object] = {}

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        if BINARY is None:
            raise RuntimeError("Pass --lemond PATH to run real-server contracts")
        cls.daemon = IsolatedLemond(
            BINARY,
            api_key=cls.api_key,
            admin_key=cls.admin_key,
            options=dict(cls.server_options),
        )
        cls.daemon.start()
        cls.addClassCleanup(cls.daemon.close)
        cls.api = cls.daemon.api

    def assert_status(self, response, status: int):
        self.assertEqual(response.status, status, error_text(response))
        return response


class ManifestContract(LiveBase, unittest.TestCase):
    # The GUI3 bridge registers a definition but must not require llama-server.
    server_options = {"disable_model_filtering": True}

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        response = cls.api.get("/internal/routes")
        if response.status != 200:
            raise AssertionError(f"No /internal/routes: {response.status}")
        cls.routes = response.json

    def test_manifest_has_all_route_families(self):
        self.assertGreaterEqual(len(self.routes), 80)
        families = {route["id"].split(".", 1)[0] for route in self.routes}
        self.assertTrue(
            {
                "internal",
                "lemonade",
                "openai",
                "ollama",
                "anthropic",
                "mcp",
                "router",
                "llamacpp",
            }
            <= families
        )

    def test_manifest_has_no_invalid_definitions(self):
        self.assertEqual(validate_routes(self.routes), [])

    def test_generated_doc_sections_cover_runtime_manifest(self):
        self.assertEqual(validate_docs(self.routes, ROOT), [])

    def test_filtered_manifest_returns_unchanged_route(self):
        for name in (
            "internal.routes",
            "lemonade.health",
            "openai.chat_completions",
            "ollama.tags",
        ):
            with self.subTest(route=name):
                filtered = self.assert_status(
                    self.api.get("/internal/routes?id=" + urllib.parse.quote(name)), 200
                ).json
                self.assertEqual(filtered, [route_by_id(self.routes, name)])

    def test_filtered_manifest_unknown_id_is_empty(self):
        self.assertEqual(
            self.assert_status(
                self.api.get("/internal/routes?id=not.a.real.route"), 200
            ).json,
            [],
        )

    def test_filtered_manifest_does_not_match_substrings(self):
        self.assertEqual(
            self.assert_status(self.api.get("/internal/routes?id=lemonade"), 200).json,
            [],
        )

    def test_route_query_handles_percent_encoding(self):
        self.assertEqual(
            self.assert_status(
                self.api.get("/internal/routes?id=%6cemonade.health"), 200
            ).json,
            [route_by_id(self.routes, "lemonade.health")],
        )

    def test_openai_and_lemonade_use_four_prefixes(self):
        route = route_by_id(self.routes, "lemonade.health")
        self.assertEqual(route["prefixes"], "Quad")
        self.assertEqual(
            sorted(expanded_paths(route)),
            sorted(prefix + "health" for prefix in QUAD),
        )

    def test_post_only_quad_routes_reject_get(self):
        methods_by_path = {}
        for route in self.routes:
            if route["prefixes"] != "Quad":
                continue
            for path in expanded_paths(route):
                methods_by_path.setdefault(path, set()).update(route["methods"])
        paths = sorted(
            path for path, methods in methods_by_path.items() if methods == {"POST"}
        )
        self.assertGreaterEqual(len(paths), 12)
        for path in paths:
            route = next(
                route
                for route in self.routes
                if route["prefixes"] == "Quad" and path in expanded_paths(route)
            )
            with self.subTest(path=path):
                url = sample_path(route, path)
                response = self.api.get(url)
                self.assertEqual(response.status, 405, (url, error_text(response)))

    def test_every_json_route_rejects_syntactically_invalid_json(self):
        candidates = [
            route
            for route in self.routes
            if "POST" in route["methods"]
            and route["request_format"] in ("Json", "OptionalJson")
            and route["id"] != "internal.shutdown"
        ]
        self.assertGreaterEqual(len(candidates), 30)
        for route in candidates:
            url = sample_path(route, expanded_paths(route)[0])
            with self.subTest(id=route["id"], url=url):
                reply = self.api.post(
                    url, raw=b'{"broken":', headers={"Content-Type": "application/json"}
                )
                self.assertEqual(reply.status, 400, (route["id"], error_text(reply)))

    def test_every_json_route_rejects_root_arrays(self):
        candidates = [
            route
            for route in self.routes
            if "POST" in route["methods"]
            and route["request_format"] in ("Json", "OptionalJson")
            and route["id"] != "internal.shutdown"
        ]
        for route in candidates:
            url = sample_path(route, expanded_paths(route)[0])
            with self.subTest(id=route["id"], url=url):
                reply = self.api.post(url, data=[])
                self.assertEqual(reply.status, 400, (route["id"], error_text(reply)))

    def test_required_json_args_reject_wrong_types(self):
        cases = []
        for route in self.routes:
            if "POST" not in route["methods"] or route["request_format"] != "Json":
                continue
            if route["id"] == "internal.shutdown":
                continue
            required = [
                arg
                for arg in route["args"]
                if arg["in"] == "JsonBody"
                and arg["required"]
                and not arg["forwarded"]
                and isinstance(arg["schema"].get("type"), str)
            ]
            if required:
                cases.append((route, required[0]))
        self.assertGreaterEqual(len(cases), 12)
        for route, arg in cases:
            bad_value = [] if arg["schema"]["type"] != "array" else "bad-array"
            url = sample_path(route, expanded_paths(route)[0])
            with self.subTest(id=route["id"], arg=arg["name"]):
                response = self.api.post(url, data={arg["name"]: bad_value})
                self.assertEqual(
                    response.status, 400, (route["id"], error_text(response))
                )

    def test_empty_body_is_rejected_by_json_route(self):
        response = self.api.post("/api/v1/models/register", raw=b"")
        self.assertEqual(response.status, 400, error_text(response))

    def test_json_literal_null_is_rejected_by_required_json_route(self):
        self.assert_status(self.api.post("/api/v1/models/register", data=None), 400)

    def test_optional_json_unload_accepts_empty_and_null(self):
        for body in (b"", b"null", b"{}"):
            with self.subTest(body=body):
                response = self.api.post(
                    "/api/v1/unload",
                    raw=body,
                    headers={"Content-Type": "application/json"},
                )
                self.assert_status(response, 200)
                self.assertEqual(response.json["status"], "success")

    def test_openai_missing_model_has_client_error(self):
        inputs = {
            "chat/completions": {"messages": [{"role": "user", "content": "hello"}]},
            "completions": {"prompt": "hello"},
            "embeddings": {"input": "hello"},
            "responses": {"input": "hello"},
        }
        for path, payload in inputs.items():
            with self.subTest(path=path):
                res = self.api.post("/api/v1/" + path, data=payload)
                self.assert_status(res, 400)
                self.assertIn("model", error_text(res).lower())

    def test_openai_rejects_non_string_model(self):
        valid_inputs = {
            "chat/completions": {"messages": [{"role": "user", "content": "hi"}]},
            "completions": {"prompt": "hello"},
            "embeddings": {"input": "hello"},
            "responses": {"input": "hello"},
        }
        for endpoint, payload in valid_inputs.items():
            with self.subTest(endpoint=endpoint):
                res = self.api.post(
                    "/api/v1/" + endpoint, data={**payload, "model": ["wrong"]}
                )
                self.assert_status(res, 400)
                self.assertIn("model", error_text(res).lower())

    def test_unsupported_ollama_write_operations_do_not_succeed(self):
        cases = ("/api/create", "/api/copy", "/api/push")
        for path in cases:
            with self.subTest(path=path):
                response = self.api.post(path, data={})
                self.assertNotEqual(response.status, 200, path)
                self.assertLess(response.status, 600)

    def test_known_read_only_endpoints(self):
        paths = (
            "/api/v1/health",
            "/api/v1/models?show_all=true",
            "/api/v1/downloads",
            "/api/v1/jobs",
            "/api/v1/stats",
            "/api/v1/system-stats",
            "/internal/config",
            "/internal/config/defaults",
            "/internal/aliases",
            "/api/version",
            "/api/tags",
        )
        for path in paths:
            with self.subTest(path=path):
                response = self.api.get(path)
                self.assert_status(response, 200)
                self.assertIn("application/json", response.header("content-type"))
                self.assertIsInstance(response.json, (list, dict))

    def test_ollama_version_conforms_to_declared_schema(self):
        reply = self.assert_status(self.api.get("/api/version"), 200)
        spec = route_by_id(self.routes, "ollama.version")
        self.assertEqual(check_json(spec["responses"][0]["schema"], reply.json), [])

    def test_health_response_conforms_to_declared_schema(self):
        reply = self.assert_status(self.api.get("/api/v1/health"), 200)
        spec = route_by_id(self.routes, "lemonade.health")
        self.assertEqual(check_json(spec["responses"][0]["schema"], reply.json), [])

    def test_jobs_and_downloads_conform_to_declared_schema(self):
        for key, path in (
            ("lemonade.jobs_list", "/api/v1/jobs"),
            ("lemonade.downloads", "/api/v1/downloads"),
        ):
            with self.subTest(route=key):
                reply = self.assert_status(self.api.get(path), 200)
                schema = route_by_id(self.routes, key)["responses"][0]["schema"]
                self.assertEqual(check_json(schema, reply.json), [])

    def test_four_health_prefixes_agree_on_status(self):
        for prefix in QUAD:
            with self.subTest(prefix=prefix):
                self.assertEqual(
                    self.assert_status(self.api.get(prefix + "health"), 200).json[
                        "status"
                    ],
                    "ok",
                )

    def test_four_model_prefixes_return_identical_public_ids(self):
        model_sets = []
        for prefix in QUAD:
            reply = self.assert_status(
                self.api.get(prefix + "models?show_all=true"), 200
            )
            self.assertIsInstance(reply.json["data"], list)
            model_sets.append({row["id"] for row in reply.json["data"]})
        self.assertTrue(model_sets[0])
        self.assertTrue(all(models == model_sets[0] for models in model_sets))

    def test_unknown_url_gives_json_404(self):
        reply = self.api.get("/api/v1/http-refactor-nonexistent-route")
        self.assert_status(reply, 404)
        self.assertIn("error", reply.json)

    def test_head_health_has_no_body(self):
        reply = self.api.request("HEAD", "/api/v1/health")
        self.assert_status(reply, 200)
        self.assertEqual(reply.body, b"")

    def test_metrics_endpoint_has_prometheus_content_type(self):
        reply = self.assert_status(self.api.get("/metrics"), 200)
        self.assertIn("text/plain", reply.header("content-type"))

    def test_mcp_get_rejects_with_post_hint(self):
        reply = self.api.get("/mcp")
        self.assert_status(reply, 405)
        self.assertEqual(reply.header("allow"), "POST")

    def test_missing_multipart_upload_gets_client_error(self):
        reply = self.api.post(
            "/api/v1/audio/transcriptions", data={"model": "faux-model"}
        )
        self.assert_status(reply, 400)

    def test_form_temperature_invalid_integer_string_gets_client_error(self):
        raw, headers = multipart({"model": "not-installed", "temperature": "wrong"})
        reply = self.api.post("/api/v1/audio/transcriptions", raw=raw, headers=headers)
        self.assert_status(reply, 400)
        self.assertIn("temperature", error_text(reply))

    def test_form_nan_and_infinity_get_client_errors(self):
        for value in ("nan", "inf", "1e100000"):
            with self.subTest(value=value):
                raw, headers = multipart(
                    {"model": "not-installed", "temperature": value}
                )
                reply = self.api.post(
                    "/api/v1/audio/transcriptions", raw=raw, headers=headers
                )
                self.assert_status(reply, 400)

    def test_form_missing_file_rejected_before_loading_model(self):
        raw, headers = multipart({"model": "not-installed", "temperature": "0.7"})
        reply = self.api.post("/api/v1/audio/transcriptions", raw=raw, headers=headers)
        self.assert_status(reply, 400)
        self.assertIn("file", error_text(reply).lower())

    def test_shared_get_post_route_dispatches_to_correct_handler(self):
        before = self.assert_status(self.api.get("/api/v1/jobs"), 200).json["jobs"]
        reply = self.api.post("/api/v1/jobs", data={"definition": {"steps": []}})
        self.assert_status(reply, 400)
        after = self.assert_status(self.api.get("/api/v1/jobs"), 200).json["jobs"]
        self.assertEqual(after, before)

    def test_four_prefixes_reach_registration_validation(self):
        for prefix in QUAD:
            with self.subTest(prefix=prefix):
                reply = self.api.post(
                    prefix + "models/register",
                    data={"model_name": "user.HttpRefactorMissingRecipe"},
                )
                self.assert_status(reply, 400)
                self.assertIn("recipe", error_text(reply).lower())

    def test_non_object_json_scalars_are_rejected(self):
        for value in (True, 10, "string", None):
            with self.subTest(value=value):
                self.assert_status(self.api.post("/internal/aliases", data=value), 400)

    def test_json_number_overflow_is_a_client_error(self):
        payload = b'{"ctx_size": 1e100000000}'
        for path in ("/internal/set", "/api/v1/models/register"):
            with self.subTest(path=path):
                self.assert_status(
                    self.api.post(
                        path, raw=payload, headers={"Content-Type": "application/json"}
                    ),
                    400,
                )
        self.assert_status(self.api.get("/api/v1/health"), 200)

    def test_invalid_numeric_query_arguments(self):
        for value in ("abc", "18446744073709551616"):
            with self.subTest(sync_id=value):
                self.assert_status(
                    self.api.get("/internal/models/sync/status?sync_id=" + value), 400
                )

    def test_registry_search_validation_stops_before_network(self):
        for query in (
            "",
            "?query=hi",
            "?query=qwen&limit=0",
            "?query=qwen&limit=51",
            "?query=qwen&limit=bad",
        ):
            with self.subTest(query=query):
                self.assert_status(self.api.get("/api/v1/registry/search" + query), 400)
        reply = self.assert_status(
            self.api.get("/api/v1/registry/search?query=qwen"), 400
        )
        self.assertIn("offline", error_text(reply).lower())

    def test_additional_read_responses_match_registered_schemas(self):
        cases = (
            ("openai.models", "/api/v1/models?show_all=true"),
            ("ollama.tags", "/api/tags"),
            ("internal.aliases_list", "/internal/aliases"),
            ("internal.models_sync_status", "/internal/models/sync/status"),
            ("lemonade.stats", "/api/v1/stats"),
            ("lemonade.system_stats", "/api/v1/system-stats"),
            ("lemonade.system_info", "/api/v1/system-info"),
        )
        for route_id, path in cases:
            with self.subTest(route=route_id):
                reply = self.assert_status(self.api.get(path), 200)
                schema = route_by_id(self.routes, route_id)["responses"][0]["schema"]
                self.assertEqual(check_json(schema, reply.json), [])

    def test_form_invalid_response_format_with_file_does_not_load(self):
        raw, headers = multipart(
            {"model": "not-installed", "response_format": "invalid"},
            {"file": ("tiny.wav", b"RIFF" + b"\0" * 32)},
        )
        reply = self.api.post("/api/v1/audio/transcriptions", raw=raw, headers=headers)
        self.assert_status(reply, 400)
        self.assertIn("response_format", error_text(reply))

    def test_form_invalid_temperature_with_file_does_not_load(self):
        raw, headers = multipart(
            {"model": "not-installed", "temperature": "invalid-number"},
            {"file": ("tiny.wav", b"RIFF" + b"\0" * 32)},
        )
        reply = self.api.post("/api/v1/audio/transcriptions", raw=raw, headers=headers)
        self.assert_status(reply, 400)
        self.assertIn("temperature", error_text(reply))

    def test_gui3_real_api_client_over_http(self):
        if not WITH_GUI3:
            self.skipTest("Run with --gui3 and installed src/app TypeScript")
        env = dict(os.environ)
        env.update(
            {
                "LEMONADE_HTTP_REFACTOR_ISOLATED": "1",
                "LEMONADE_HTTP_REFACTOR_URL": f"http://127.0.0.1:{self.daemon.port}",
            }
        )
        result = subprocess.run(
            ["node", str(HERE / "gui3_bridge.cjs")],
            env=env,
            cwd=ROOT,
            text=True,
            capture_output=True,
            timeout=90,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + "\n" + result.stderr)
        self.assertIn("GUI3 real API checks passed", result.stdout)


class SecurityContract(LiveBase, unittest.TestCase):
    api_key = "api-refactor-secret"
    admin_key = "admin-refactor-secret"

    def test_regular_endpoints_reject_missing_token(self):
        for path in (
            "/api/v1/health",
            "/v1/models",
            "/api/version",
            "/api/tags",
            "/metrics",
        ):
            with self.subTest(path=path):
                self.assert_status(self.daemon.anonymous.get(path), 401)

    def test_internal_routes_reject_missing_token(self):
        for path in ("/internal/config", "/internal/routes", "/internal/aliases"):
            with self.subTest(path=path):
                self.assert_status(self.daemon.anonymous.get(path), 401)

    def test_regular_key_cannot_access_internal(self):
        self.assert_status(self.api.get("/internal/routes"), 401)

    def test_regular_key_can_access_api(self):
        self.assert_status(self.api.get("/api/v1/health"), 200)
        self.assert_status(self.api.get("/api/version"), 200)

    def test_admin_key_can_access_both(self):
        self.assert_status(self.daemon.admin.get("/internal/routes"), 200)
        self.assert_status(self.daemon.admin.get("/api/v1/health"), 200)
        self.assert_status(self.daemon.admin.get("/metrics"), 200)

    def test_wrong_credentials_never_grant_access(self):
        other = Client(self.daemon.port, "incorrect-secret")
        for path in ("/internal/routes", "/api/v1/health", "/api/tags"):
            with self.subTest(path=path):
                self.assert_status(other.get(path), 401)

    def test_malformed_bearer_headers_do_not_crash(self):
        for value in ("Bearer", "Bearer ", "Basic example", "", "x"):
            with self.subTest(header=value):
                reply = self.daemon.anonymous.get(
                    "/api/v1/health", headers={"Authorization": value}
                )
                self.assert_status(reply, 401)
        self.assert_status(self.daemon.admin.get("/api/v1/health"), 200)

    def test_mcp_gateway_requires_regular_auth(self):
        self.assert_status(self.daemon.anonymous.post("/mcp", data={}), 401)
        reply = self.api.post("/mcp", data={})
        self.assertNotEqual(reply.status, 401, error_text(reply))

    def test_authentication_is_checked_before_bad_json(self):
        reply = self.daemon.anonymous.post("/v1/chat/completions", raw=b"{")
        self.assert_status(reply, 401)
        reply = self.api.post("/v1/chat/completions", raw=b"{")
        self.assert_status(reply, 400)

    def test_preflight_does_not_require_api_token(self):
        for path in ("/api/v1/health", "/internal/routes", "/mcp"):
            with self.subTest(path=path):
                reply = self.daemon.anonymous.request("OPTIONS", path)
                self.assert_status(reply, 204)

    def test_foreign_origin_is_blocked_before_route(self):
        for path in ("/api/v1/health", "/internal/routes", "/api/version"):
            with self.subTest(path=path):
                reply = self.daemon.admin.get(
                    path, headers={"Origin": "https://untrusted.invalid"}
                )
                self.assert_status(reply, 403)
                self.assertNotIn(
                    "https://untrusted.invalid",
                    reply.header("access-control-allow-origin"),
                )
                self.assertIn("Origin", reply.header("vary"))

    def test_same_origin_is_accepted(self):
        reply = self.daemon.admin.get(
            "/api/v1/health",
            headers={"Origin": f"http://127.0.0.1:{self.daemon.port}"},
        )
        self.assert_status(reply, 200)
        self.assertIn("Origin", reply.header("vary"))

    def test_server_survives_failed_auth_requests(self):
        for _ in range(10):
            self.assert_status(self.daemon.anonymous.get("/api/v1/health"), 401)
        self.assert_status(self.daemon.admin.get("/internal/routes"), 200)

    def test_authentication_precedes_method_not_allowed(self):
        for path in ("/v1/chat/completions", "/api/v1/models/register"):
            with self.subTest(path=path):
                self.assert_status(self.daemon.anonymous.get(path), 401)
                self.assert_status(self.api.get(path), 405)

    def test_unknown_api_route_is_still_protected(self):
        path = "/api/v1/unknown-http-refactor-path"
        self.assert_status(self.daemon.anonymous.get(path), 401)
        self.assert_status(self.api.get(path), 404)

    def test_non_admin_cannot_create_alias(self):
        alias = "httprefactor-auth-" + uuid.uuid4().hex[:10]
        data = {"alias": alias, "target": "user.none"}
        self.assert_status(
            self.daemon.anonymous.post("/internal/aliases", data=data), 401
        )
        self.assert_status(self.api.post("/internal/aliases", data=data), 401)
        aliases = self.assert_status(
            self.daemon.admin.get("/internal/aliases"), 200
        ).json["aliases"]
        self.assertFalse(any(row["alias"] == alias for row in aliases))

    def test_preflight_exposes_required_api_headers_for_local_origin(self):
        origin = f"http://127.0.0.1:{self.daemon.port}"
        reply = self.daemon.anonymous.request(
            "OPTIONS",
            "/api/v1/models",
            headers={"Origin": origin, "Access-Control-Request-Method": "POST"},
        )
        self.assert_status(reply, 204)
        self.assertEqual(reply.header("access-control-allow-origin"), origin)
        self.assertIn("Authorization", reply.header("access-control-allow-headers"))


class AdminOnlyContract(LiveBase, unittest.TestCase):
    admin_key = "admin-only-secret"

    def test_admin_only_secures_internal_but_not_api(self):
        self.assert_status(self.daemon.anonymous.get("/internal/routes"), 401)
        self.assert_status(self.daemon.anonymous.get("/api/v1/health"), 200)


class StateAndConcurrencyContract(LiveBase, unittest.TestCase):
    # Keep register-only contracts independent of local inference backends.
    server_options = {"disable_model_filtering": True}

    def register_offline_model(
        self,
        name: str | None = None,
        checkpoint: str = "lemonade-compat/no-network:Q4_K_M",
    ) -> tuple[str, str]:
        name = name or "user.HttpRefactor" + uuid.uuid4().hex[:12]
        result = self.assert_status(
            self.api.post(
                "/api/v1/models/register",
                data={
                    "model_name": name,
                    "recipe": "llamacpp",
                    "checkpoint": checkpoint,
                    "labels": ["chat"],
                },
            ),
            200,
        ).json
        self.assertEqual(result["canonical_model_name"], name)
        return name, result["model_name"]

    def test_alias_round_trip_through_routes(self):
        alias = "httprefactor-" + uuid.uuid4().hex[:10]
        try:
            reply = self.assert_status(
                self.api.post(
                    "/internal/aliases",
                    data={"alias": alias, "target": "user.nonexistent"},
                ),
                200,
            )
            self.assertEqual(reply.json["alias"], alias)
            rows = self.assert_status(self.api.get("/internal/aliases"), 200).json[
                "aliases"
            ]
            self.assertTrue(any(row["alias"] == alias for row in rows))
        finally:
            self.api.delete("/internal/aliases/" + alias)
        rows = self.assert_status(self.api.get("/internal/aliases"), 200).json[
            "aliases"
        ]
        self.assertFalse(any(row["alias"] == alias for row in rows))

    def test_alias_target_legacy_spelling_is_supported(self):
        alias = "httprefactor-" + uuid.uuid4().hex[:10]
        try:
            reply = self.assert_status(
                self.api.post(
                    "/internal/aliases", data={"alias": alias, "model": "user.other"}
                ),
                200,
            )
            self.assertEqual(reply.json["target"], "user.other")
        finally:
            self.api.delete("/internal/aliases/" + alias)

    def test_alias_rejects_invalid_arg_type(self):
        response = self.api.post(
            "/internal/aliases", data={"alias": 24, "target": "user.other"}
        )
        self.assert_status(response, 400)

    def test_alias_cycle_is_rejected_without_losing_original(self):
        left = "httprefactor-" + uuid.uuid4().hex[:10]
        right = "httprefactor-" + uuid.uuid4().hex[:10]
        try:
            self.assert_status(
                self.api.post(
                    "/internal/aliases", data={"alias": left, "target": right}
                ),
                200,
            )
            conflict = self.api.post(
                "/internal/aliases", data={"alias": right, "target": left}
            )
            self.assert_status(conflict, 409)
            entries = self.api.get("/internal/aliases").json["aliases"]
            self.assertTrue(
                any(e["alias"] == left and e["target"] == right for e in entries)
            )
            self.assertFalse(any(e["alias"] == right for e in entries))
        finally:
            self.api.delete("/internal/aliases/" + left)
            self.api.delete("/internal/aliases/" + right)

    def test_model_register_list_detail_options_delete(self):
        name = "user.HttpRefactor" + uuid.uuid4().hex[:12]
        path_name = urllib.parse.quote(name, safe="")
        try:
            _, public_id = self.register_offline_model(name)
            escaped = urllib.parse.quote(public_id, safe="")
            rows = self.assert_status(
                self.api.get("/api/v1/models?show_all=true"), 200
            ).json["data"]
            self.assertTrue(any(row["id"] == public_id for row in rows))
            detail = self.assert_status(
                self.api.get("/api/v1/models/" + escaped), 200
            ).json
            self.assertEqual(detail["checkpoint"], "lemonade-compat/no-network:Q4_K_M")
            original = self.assert_status(
                self.api.get("/api/v1/models/" + escaped + "/options"), 200
            ).json
            self.assertIsInstance(original["saved"], dict)
            updated = self.assert_status(
                self.api.post(
                    "/api/v1/models/" + escaped + "/options", data={"ctx_size": 4096}
                ),
                200,
            ).json
            self.assertEqual(updated["saved"]["ctx_size"], 4096)
            restored = self.assert_status(
                self.api.delete("/api/v1/models/" + escaped + "/options"), 200
            ).json
            self.assertNotIn("ctx_size", restored["saved"])
        finally:
            self.api.post("/api/v1/delete", data={"model_name": name})
        self.assertNotEqual(self.api.get("/api/v1/models/" + path_name).status, 200)

    def test_config_change_preserves_other_keys(self):
        before = self.assert_status(self.api.get("/internal/config"), 200).json
        old = before["log_level"]
        new = "debug" if old != "debug" else "info"
        try:
            result = self.assert_status(
                self.api.post("/internal/set", data={"log_level": new}), 200
            )
            self.assertIsInstance(result.json, dict)
            after = self.api.get("/internal/config").json
            self.assertEqual(after["log_level"], new)
            self.assertEqual(after["models_dir"], before["models_dir"])
        finally:
            self.api.post("/internal/set", data={"log_level": old})

    def test_config_change_rejects_bad_type(self):
        before = self.api.get("/internal/config").json
        reply = self.api.post(
            "/internal/set", data={"ctx_size": "definitely-not-integer"}
        )
        self.assert_status(reply, 400)
        after = self.api.get("/internal/config").json
        self.assertEqual(after["ctx_size"], before["ctx_size"])

    def test_offline_registration_uses_explicit_test_only_filter_override(self):
        config = self.assert_status(self.api.get("/internal/config"), 200).json
        self.assertTrue(config["disable_model_filtering"])
        self.assertTrue(config["offline"])
        self.assertTrue(config["no_fetch_executables"])
        self.assertEqual(Path(config["models_dir"]), self.daemon.models)

    def test_registration_rejects_nested_invalid_values_without_mutating_inventory(
        self,
    ):
        bad_definitions = (
            {"recipe": "llamacpp", "checkpoint": "a/b:Q4_K_M", "labels": ["chat", 7]},
            {
                "recipe": "llamacpp",
                "checkpoint": "a/b:Q4_K_M",
                "checkpoints": {"main": 4},
            },
            {
                "recipe": "llamacpp",
                "checkpoint": "a/b:Q4_K_M",
                "source": "not-a-registry",
            },
            {
                "recipe": "llamacpp",
                "checkpoint": "a/b:Q4_K_M",
                "checkpoints": {"aux": "x/y"},
            },
            {
                "recipe": "llamacpp",
                "checkpoint": "a/b:Q4_K_M",
                "components": ["valid", 3],
            },
        )
        for fields in bad_definitions:
            with self.subTest(fields=fields):
                name = "user.InvalidHttpRefactor" + uuid.uuid4().hex[:12]
                reply = self.api.post(
                    "/api/v1/models/register", data={"model_name": name, **fields}
                )
                self.assert_status(reply, 400)
                rows = self.assert_status(
                    self.api.get("/api/v1/models?show_all=true"), 200
                ).json["data"]
                self.assertFalse(any(row["id"] == name for row in rows))

    def test_registration_rejects_reserved_names(self):
        for name in (
            "model-without-user-prefix",
            "extra.http-refactor",
            "builtin.http-refactor",
            "user.",
        ):
            with self.subTest(name=name):
                reply = self.api.post(
                    "/api/v1/models/register",
                    data={
                        "model_name": name,
                        "recipe": "llamacpp",
                        "checkpoint": "a/b:Q4_K_M",
                    },
                )
                self.assert_status(reply, 400)

    def test_model_options_validation_is_atomic(self):
        name, public_id = self.register_offline_model()
        endpoint = (
            "/api/v1/models/" + urllib.parse.quote(public_id, safe="") + "/options"
        )
        try:
            self.assert_status(self.api.post(endpoint, data={"ctx_size": 4096}), 200)
            invalid = (
                {"ctx_size": 0},
                {"ctx_size": -2},
                {"ctx_size": 3.5},
                {"ctx_size": "a lot"},
                {"ctx_size": 8192, "unknown_http_refactor_option": True},
            )
            for changes in invalid:
                with self.subTest(changes=changes):
                    self.assert_status(self.api.post(endpoint, data=changes), 400)
                    saved = self.assert_status(self.api.get(endpoint), 200).json[
                        "saved"
                    ]
                    self.assertEqual(saved["ctx_size"], 4096)
                    self.assertNotIn("unknown_http_refactor_option", saved)
        finally:
            self.api.post("/api/v1/delete", data={"model_name": name})

    def test_model_options_dry_run_does_not_persist(self):
        name, public_id = self.register_offline_model()
        endpoint = (
            "/api/v1/models/" + urllib.parse.quote(public_id, safe="") + "/options"
        )
        try:
            before = self.assert_status(self.api.get(endpoint), 200).json["saved"]
            trial = self.assert_status(
                self.api.post(endpoint, data={"ctx_size": 8192, "dry_run": True}), 200
            ).json
            self.assertEqual(trial["saved"], before)
            self.assertEqual(
                self.assert_status(self.api.get(endpoint), 200).json["saved"], before
            )
        finally:
            self.api.post("/api/v1/delete", data={"model_name": name})

    def test_files_list_does_not_expose_paths_by_default(self):
        name, public_id = self.register_offline_model()
        endpoint = "/api/v1/models/" + urllib.parse.quote(public_id, safe="") + "/files"
        try:
            reply = self.assert_status(self.api.get(endpoint), 200).json
            self.assertEqual(reply["model_id"], public_id)
            for item in reply["files"]:
                self.assertNotIn("path", item)
            details = self.assert_status(
                self.api.get(endpoint + "?include_paths=true"), 200
            ).json
            self.assertEqual(
                [(row["name"], row["role"]) for row in reply["files"]],
                [(row["name"], row["role"]) for row in details["files"]],
            )
        finally:
            self.api.post("/api/v1/delete", data={"model_name": name})

    def test_alias_delete_reports_missing_alias_after_first_delete(self):
        alias = "httprefactor-" + uuid.uuid4().hex[:12]
        self.assert_status(self.api.delete("/internal/aliases/" + alias), 404)
        try:
            self.assert_status(
                self.api.post(
                    "/internal/aliases", data={"alias": alias, "target": "user.none"}
                ),
                200,
            )
            self.assert_status(self.api.delete("/internal/aliases/" + alias), 200)
            self.assert_status(self.api.delete("/internal/aliases/" + alias), 404)
        finally:
            self.api.delete("/internal/aliases/" + alias)

    def test_alias_rejects_reserved_and_self_references_without_side_effects(self):
        alias = "httprefactor-" + uuid.uuid4().hex[:12]
        for data in (
            {"alias": alias, "target": alias},
            {"alias": "user." + alias, "target": "user.other"},
            {"alias": "extra." + alias, "target": "user.other"},
        ):
            with self.subTest(data=data):
                self.assert_status(self.api.post("/internal/aliases", data=data), 400)
        rows = self.assert_status(self.api.get("/internal/aliases"), 200).json[
            "aliases"
        ]
        self.assertFalse(any(row["alias"].endswith(alias) for row in rows))

    def test_alias_update_replaces_target_without_duplicating_entry(self):
        alias = "httprefactor-" + uuid.uuid4().hex[:12]
        try:
            for target in ("user.first", "user.second"):
                reply = self.api.post(
                    "/internal/aliases", data={"alias": alias, "target": target}
                )
                self.assert_status(reply, 200)
            entries = self.assert_status(self.api.get("/internal/aliases"), 200).json[
                "aliases"
            ]
            matches = [row for row in entries if row["alias"] == alias]
            self.assertEqual(len(matches), 1)
            self.assertEqual(matches[0]["target"], "user.second")
        finally:
            self.api.delete("/internal/aliases/" + alias)

    def test_alias_target_can_be_read_using_public_model_api(self):
        name, public_id = self.register_offline_model()
        alias = "httprefactor-" + uuid.uuid4().hex[:12]
        try:
            self.assert_status(
                self.api.post(
                    "/internal/aliases", data={"alias": alias, "target": name}
                ),
                200,
            )
            self.assertNotEqual(name, public_id)
            aliased = self.assert_status(
                self.api.get("/api/v1/models/" + alias), 200
            ).json
            self.assertEqual(aliased["id"], alias)
            self.assertEqual(aliased["checkpoint"], "lemonade-compat/no-network:Q4_K_M")
            items = self.assert_status(
                self.api.get("/api/v1/models?show_all=true"), 200
            ).json["data"]
            self.assertIn(public_id, {item["id"] for item in items})
            matches = [item for item in items if item["id"] == alias]
            self.assertEqual(len(matches), 1)
            self.assertEqual(matches[0]["checkpoint"], aliased["checkpoint"])
            self.assertEqual(matches[0]["recipe"], aliased["recipe"])
            self.assertFalse(matches[0]["downloaded"])
            downloaded = self.assert_status(self.api.get("/api/v1/models"), 200).json[
                "data"
            ]
            self.assertNotIn(alias, {item["id"] for item in downloaded})
        finally:
            self.api.delete("/internal/aliases/" + alias)
            self.api.post("/api/v1/delete", data={"model_name": name})

    def test_alias_targeting_public_user_name_is_listed(self):
        name, public_id = self.register_offline_model()
        alias = "httprefactor-" + uuid.uuid4().hex[:12]
        try:
            self.assertNotEqual(name, public_id)
            self.assert_status(
                self.api.post(
                    "/internal/aliases", data={"alias": alias, "target": public_id}
                ),
                200,
            )
            rows = self.assert_status(
                self.api.get("/api/v1/models?show_all=true"), 200
            ).json["data"]
            matches = [row for row in rows if row["id"] == alias]
            self.assertEqual(len(matches), 1)
            details = self.assert_status(
                self.api.get("/api/v1/models/" + alias), 200
            ).json
            self.assertEqual(details["id"], alias)
            self.assertEqual(matches[0]["checkpoint"], details["checkpoint"])
        finally:
            self.api.delete("/internal/aliases/" + alias)
            self.api.post("/api/v1/delete", data={"model_name": name})

    def test_alias_chain_to_user_model_is_listed_once_per_alias(self):
        name, public_id = self.register_offline_model()
        first = "httprefactor-" + uuid.uuid4().hex[:12]
        second = "httprefactor-" + uuid.uuid4().hex[:12]
        try:
            self.assert_status(
                self.api.post(
                    "/internal/aliases", data={"alias": second, "target": name}
                ),
                200,
            )
            self.assert_status(
                self.api.post(
                    "/internal/aliases", data={"alias": first, "target": second}
                ),
                200,
            )
            items = self.assert_status(
                self.api.get("/api/v1/models?show_all=true"), 200
            ).json["data"]
            for alias in (first, second):
                with self.subTest(alias=alias):
                    matches = [row for row in items if row["id"] == alias]
                    self.assertEqual(len(matches), 1)
                    self.assertEqual(
                        matches[0]["checkpoint"], "lemonade-compat/no-network:Q4_K_M"
                    )
                    self.assertEqual(
                        self.assert_status(
                            self.api.get("/v1/models/" + alias), 200
                        ).json["id"],
                        alias,
                    )
        finally:
            self.api.delete("/internal/aliases/" + first)
            self.api.delete("/internal/aliases/" + second)
            self.api.post("/api/v1/delete", data={"model_name": name})

    def test_alias_listing_tracks_retargeting_and_deletion(self):
        first_name, first_public = self.register_offline_model()
        second_name = "user.HttpRefactor" + uuid.uuid4().hex[:12]
        second_registered = False
        alias = "httprefactor-" + uuid.uuid4().hex[:12]
        try:
            _, second_public = self.register_offline_model(
                second_name, checkpoint="lemonade-compat/no-network:Q5_K_M"
            )
            second_registered = True
            for target, expected_checkpoint in (
                (first_name, "lemonade-compat/no-network:Q4_K_M"),
                (second_name, "lemonade-compat/no-network:Q5_K_M"),
            ):
                with self.subTest(target=target):
                    self.assert_status(
                        self.api.post(
                            "/internal/aliases", data={"alias": alias, "target": target}
                        ),
                        200,
                    )
                    rows = self.assert_status(
                        self.api.get("/api/v1/models?show_all=true"), 200
                    ).json["data"]
                    matches = [row for row in rows if row["id"] == alias]
                    self.assertEqual(len(matches), 1)
                    self.assertEqual(matches[0]["checkpoint"], expected_checkpoint)
                    self.assertIn(first_public, {row["id"] for row in rows})
                    self.assertIn(second_public, {row["id"] for row in rows})
            self.assert_status(self.api.delete("/internal/aliases/" + alias), 200)
            rows = self.assert_status(
                self.api.get("/api/v1/models?show_all=true"), 200
            ).json["data"]
            self.assertNotIn(alias, {row["id"] for row in rows})
        finally:
            self.api.delete("/internal/aliases/" + alias)
            self.api.post("/api/v1/delete", data={"model_name": first_name})
            if second_registered:
                self.api.post("/api/v1/delete", data={"model_name": second_name})

    def test_unresolved_alias_is_not_in_public_inventory(self):
        alias = "httprefactor-" + uuid.uuid4().hex[:12]
        target = "user.MissingHttpRefactor" + uuid.uuid4().hex[:12]
        try:
            self.assert_status(
                self.api.post(
                    "/internal/aliases", data={"alias": alias, "target": target}
                ),
                200,
            )
            registered = self.assert_status(
                self.api.get("/internal/aliases"), 200
            ).json["aliases"]
            self.assertTrue(any(row["alias"] == alias for row in registered))
            for path in ("/api/v1/models?show_all=true", "/api/v1/models"):
                with self.subTest(path=path):
                    rows = self.assert_status(self.api.get(path), 200).json["data"]
                    self.assertNotIn(alias, {row["id"] for row in rows})
            self.assert_status(self.api.get("/api/v1/models/" + alias), 404)
        finally:
            self.api.delete("/internal/aliases/" + alias)

    def test_catalog_model_alias_is_listed_when_target_is_listed(self):
        items = self.assert_status(
            self.api.get("/api/v1/models?show_all=true"), 200
        ).json["data"]
        builtin = next(
            (
                row
                for row in items
                if not row["id"].startswith(("user.", "httprefactor-", "HttpRefactor"))
            ),
            None,
        )
        self.assertIsNotNone(builtin, "No catalog models available in isolated daemon")
        alias = "httprefactor-" + uuid.uuid4().hex[:12]
        try:
            self.assert_status(
                self.api.post(
                    "/internal/aliases", data={"alias": alias, "target": builtin["id"]}
                ),
                200,
            )
            rows = self.assert_status(
                self.api.get("/api/v1/models?show_all=true"), 200
            ).json["data"]
            matches = [row for row in rows if row["id"] == alias]
            self.assertEqual(len(matches), 1)
            self.assertEqual(matches[0]["checkpoint"], builtin["checkpoint"])
            self.assertEqual(matches[0]["recipe"], builtin["recipe"])
        finally:
            self.api.delete("/internal/aliases/" + alias)

    def test_alias_inventory_matches_all_quad_prefixes(self):
        name, public_id = self.register_offline_model()
        alias = "httprefactor-" + uuid.uuid4().hex[:12]
        try:
            self.assert_status(
                self.api.post(
                    "/internal/aliases", data={"alias": alias, "target": name}
                ),
                200,
            )
            for prefix in QUAD:
                with self.subTest(prefix=prefix):
                    rows = self.assert_status(
                        self.api.get(prefix + "models?show_all=true"), 200
                    ).json["data"]
                    matches = [row for row in rows if row["id"] == alias]
                    self.assertEqual(len(matches), 1)
                    self.assertEqual(
                        matches[0]["checkpoint"], "lemonade-compat/no-network:Q4_K_M"
                    )
                    self.assertIn(public_id, {row["id"] for row in rows})
        finally:
            self.api.delete("/internal/aliases/" + alias)
            self.api.post("/api/v1/delete", data={"model_name": name})

    def test_registered_model_access_across_quad_prefixes(self):
        name, public_id = self.register_offline_model()
        escaped = urllib.parse.quote(public_id, safe="")
        try:
            for prefix in QUAD:
                with self.subTest(prefix=prefix):
                    detail = self.assert_status(
                        self.api.get(prefix + "models/" + escaped), 200
                    ).json
                    self.assertEqual(
                        detail["checkpoint"], "lemonade-compat/no-network:Q4_K_M"
                    )
        finally:
            self.api.post("/api/v1/delete", data={"model_name": name})

    def test_config_invalid_batch_does_not_partially_apply(self):
        before = self.assert_status(self.api.get("/internal/config"), 200).json
        new_level = "debug" if before["log_level"] != "debug" else "info"
        reply = self.api.post(
            "/internal/set",
            data={"log_level": new_level, "unknown_http_refactor_setting": 1},
        )
        self.assert_status(reply, 400)
        after = self.assert_status(self.api.get("/internal/config"), 200).json
        self.assertEqual(after["log_level"], before["log_level"])
        self.assertEqual(after["models_dir"], before["models_dir"])

    def test_parallel_filtered_manifest_queries(self):
        paths = [
            "/internal/routes?id=lemonade.health",
            "/internal/routes?id=openai.responses",
            "/api/v1/health",
            "/api/version",
        ] * 10
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            replies = list(pool.map(self.api.get, paths))
        for path, reply in zip(paths, replies):
            with self.subTest(path=path):
                self.assert_status(reply, 200)
        self.assert_status(self.api.get("/api/v1/health"), 200)

    def test_concurrent_alias_updates_do_not_lose_entries(self):
        tag = uuid.uuid4().hex[:10]
        names = [f"httprefactor-{tag}-{i}" for i in range(16)]
        try:

            def register(name):
                return self.api.post(
                    "/internal/aliases",
                    data={"alias": name, "target": "user.not-downloaded"},
                )

            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                replies = list(pool.map(register, names))
            for name, response in zip(names, replies):
                with self.subTest(alias=name):
                    self.assert_status(response, 200)
            aliases = self.api.get("/internal/aliases").json["aliases"]
            present = {alias["alias"] for alias in aliases}
            self.assertEqual(set(names) - present, set())
        finally:
            for name in names:
                self.api.delete("/internal/aliases/" + name)

    def test_parallel_invalid_requests_do_not_corrupt_daemon(self):
        paths = [
            "/api/v1/models/register",
            "/internal/aliases",
            "/v1/chat/completions",
            "/api/chat",
        ] * 8
        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as pool:
            replies = list(
                pool.map(
                    lambda path: self.api.post(
                        path,
                        raw=b"{invalid",
                        headers={"Content-Type": "application/json"},
                    ),
                    paths,
                )
            )
        self.assertTrue(all(reply.status == 400 for reply in replies))
        self.assert_status(self.api.get("/api/v1/health"), 200)


def iter_tests(suite):
    for entry in suite:
        if isinstance(entry, unittest.TestSuite):
            yield from iter_tests(entry)
        else:
            yield entry


def main() -> int:
    global BINARY, WITH_GUI3
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--lemond", required=True, help="Built lemond from this checkout"
    )
    parser.add_argument(
        "--gui3", action="store_true", help="Use GUI3's real TypeScript API client"
    )
    parser.add_argument(
        "--match", default="", help="Run only methods containing this substring"
    )
    parser.add_argument(
        "--quiet", action="store_true", help="Show only failure details"
    )
    args = parser.parse_args()
    BINARY = find_lemond(args.lemond)
    WITH_GUI3 = args.gui3
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    if args.match:
        suite = unittest.TestSuite(
            t for t in iter_tests(suite) if args.match.lower() in t.id().lower()
        )
        if suite.countTestCases() == 0:
            parser.error(f"No tests match {args.match!r}")
    result = unittest.TextTestRunner(verbosity=1 if args.quiet else 2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
