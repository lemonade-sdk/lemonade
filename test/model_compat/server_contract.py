"""Black-box model-manager/API compatibility tests against an ISOLATED lemond.

No inference, model download, external registry request, or user cache required.
The daemon is launched with a fresh config and model directory; all writes are
confined to a TemporaryDirectory. See README.md before changing this boundary.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
import uuid

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
LEMOND = None
CHECK_GUI3 = False


def find_lemond(requested: str | None) -> str:
    if requested:
        path = Path(requested).expanduser().resolve()
        if not path.is_file():
            raise FileNotFoundError(f"lemond not found: {path}")
        return str(path)
    for candidate in (
        ROOT / "build" / "bin" / "lemond",
        ROOT / "build" / "lemond",
        ROOT / "build" / "Release" / "lemond",
        ROOT / "build" / "Debug" / "lemond",
    ):
        if candidate.is_file():
            return str(candidate)
    installed = shutil.which("lemond")
    if installed:
        return installed
    raise FileNotFoundError(
        "Build lemond first, or specify --lemond /absolute/path/to/lemond"
    )


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class Api:
    def __init__(self, port: int, api_key: str = ""):
        self.url = f"http://127.0.0.1:{port}"
        self.api_key = api_key

    def request(self, method: str, endpoint: str, data=None, timeout=12):
        headers = {"Accept": "application/json"}
        if self.api_key:
            headers["Authorization"] = "Bearer " + self.api_key
        payload = None
        if data is not None:
            payload = json.dumps(data).encode("utf-8")
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(
            self.url + endpoint, data=payload, headers=headers, method=method
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.status, self._parse(resp.read())
        except urllib.error.HTTPError as err:
            return err.code, self._parse(err.read())

    @staticmethod
    def _parse(raw: bytes):
        if not raw:
            return None
        try:
            return json.loads(raw)
        except (ValueError, UnicodeDecodeError):
            return raw.decode("utf-8", errors="replace")

    def get(self, path):
        return self.request("GET", path)

    def post(self, path, body):
        return self.request("POST", path, body)

    def delete(self, path):
        return self.request("DELETE", path)


class IsolatedLemond:
    def __init__(self, binary: str):
        self.binary = binary
        self.tmp = tempfile.TemporaryDirectory(prefix="lemon-model-contract-")
        self.root = Path(self.tmp.name)
        self.cache = self.root / "config"
        self.models = self.root / "models"
        self.extra = self.root / "extra"
        for directory in (self.cache, self.models, self.extra):
            directory.mkdir(parents=True, exist_ok=True)
        self.port = free_port()
        self.log_path = self.root / "lemond.log"
        self.proc = None
        self.log = None
        self.api = Api(self.port)

    def start(self):
        (self.cache / "config.json").write_text(
            json.dumps(
                {
                    "config_version": 2,
                    "offline": True,
                    "auto_check_model_updates": False,
                    "models_dir": str(self.models),
                    "extra_models_dir": "",
                    "model_catalogs": [],
                }
            ),
            encoding="utf-8",
        )
        self.log = self.log_path.open("wb")
        safe_env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith("LEMONADE_")
            and key
            not in (
                "HF_HOME",
                "HF_HUB_CACHE",
                "MODELSCOPE_CACHE",
                "XDG_CACHE_HOME",
                "XDG_CONFIG_HOME",
            )
        }
        safe_env.update(
            {
                "HOME": str(self.root),
                "XDG_CACHE_HOME": str(self.root / "xdg-cache"),
                "XDG_CONFIG_HOME": str(self.root / "xdg-config"),
                "HF_HOME": str(self.root / "hf"),
                "HF_HUB_CACHE": str(self.models),
                "LEMONADE_DISABLE_TELEMETRY": "1",
            }
        )
        self.proc = subprocess.Popen(
            [self.binary, str(self.cache), "--port", str(self.port), "--no-broadcast"],
            stdin=subprocess.DEVNULL,
            stdout=self.log,
            stderr=subprocess.STDOUT,
            env=safe_env,
        )
        deadline = time.monotonic() + 50
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                break
            try:
                if self.api.get("/api/v1/health")[0] == 200:
                    status, config = self.api.get("/internal/config")
                    configured = (
                        config.get("models_dir")
                        if status == 200 and isinstance(config, dict)
                        else None
                    )
                    if configured != str(self.models):
                        self.stop()
                        raise RuntimeError(
                            f"Refusing to run: isolated models_dir was not accepted "
                            f"(expected {self.models}, got {configured!r})"
                        )
                    return
            except (OSError, TimeoutError):
                pass
            time.sleep(0.25)
        error = "lemond never became healthy; log tail:\n" + self.log_tail()
        self.stop()
        raise RuntimeError(error)

    def log_tail(self) -> str:
        if self.log:
            self.log.flush()
        if self.log_path.exists():
            return self.log_path.read_text(encoding="utf-8", errors="replace")[-7000:]
        return "(no log)"

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        if self.log:
            self.log.close()
            self.log = None

    def close(self):
        self.stop()
        self.tmp.cleanup()

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *_):
        self.close()


def encoded(name: str) -> str:
    return urllib.parse.quote(name, safe="")


def write_gguf(file: Path):
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_bytes(b"GGUF" + struct.pack("<IQQ", 3, 0, 0))


class ModelManagerApiContract(unittest.TestCase):
    """Each test mutates only the newly spawned, private daemon."""

    @classmethod
    def setUpClass(cls):
        if LEMOND is None:
            raise RuntimeError("Set --lemond or place lemond on PATH")
        cls.daemon = IsolatedLemond(LEMOND)
        cls.daemon.start()
        cls.api = cls.daemon.api
        cls.builtin = cls._builtin_name()

    @classmethod
    def tearDownClass(cls):
        cls.daemon.close()

    @classmethod
    def _builtin_name(cls):
        status, data = cls.api.get("/api/v1/models?show_all=true")
        if (
            status != 200
            or not isinstance(data, dict)
            or not isinstance(data.get("data"), list)
        ):
            raise AssertionError(f"Broken initial model list: {status} {data}")
        return next(
            (
                row["id"]
                for row in data["data"]
                if row.get("id")
                and not row.get("id", "").startswith(("user.", "extra."))
            ),
            None,
        )

    def new_name(self):
        return "user.compat-" + uuid.uuid4().hex[:12]

    def assert_ok(self, response, status=200):
        code, body = response
        self.assertEqual(code, status, body)
        return body

    def rows(self):
        body = self.assert_ok(self.api.get("/api/v1/models?show_all=true"))
        self.assertIsInstance(body, dict)
        self.assertIsInstance(body.get("data"), list)
        return body["data"]

    @contextlib.contextmanager
    def registered(self, **fields):
        name = self.new_name()
        body = {
            "model_name": name,
            "recipe": "llamacpp",
            "checkpoint": "lemonade-compat/no-network:Q4_K_M",
            "labels": ["chat"],
            **fields,
        }
        if "checkpoints" in fields:
            body.pop("checkpoint", None)
        registered = self.assert_ok(self.api.post("/api/v1/models/register", body))
        self.assertEqual(registered["canonical_model_name"], name)
        public = registered["model_name"]
        try:
            yield name, public
        finally:
            status, resp = self.api.post("/api/v1/delete", {"model_name": name})
            if status not in (200, 404):
                raise AssertionError(f"cleanup failed for {name}: {status} {resp}")

    def detail(self, name):
        return self.assert_ok(self.api.get("/api/v1/models/" + encoded(name)))

    def test_health_is_gui3_compatible(self):
        obj = self.assert_ok(self.api.get("/api/v1/health"))
        self.assertIsInstance(obj.get("status"), str)
        self.assertIsInstance(obj.get("all_models_loaded"), list)
        self.assertIsInstance(obj.get("max_models"), dict)

    def test_model_list_shape_and_unique_public_ids(self):
        models = self.rows()
        self.assertTrue(models, "Expected built-in registry entries in show_all list")
        ids = [item["id"] for item in models]
        self.assertEqual(len(ids), len(set(ids)))
        for row in models:
            self.assertIsInstance(row["id"], str)
            self.assertIsInstance(row.get("downloaded"), bool, row)
            self.assertIn("recipe", row)
            self.assertIn("checkpoint", row)

    def test_v0_v1_and_short_route_public_model_list(self):
        for p in ("/api/v0/models", "/api/v1/models", "/v1/models"):
            rows = self.assert_ok(self.api.get(p + "?show_all=true"))["data"]
            self.assertIsInstance(rows, list)
            self.assertTrue(rows)

    def test_known_model_detail_matches_list(self):
        if not self.builtin:
            self.skipTest("No built-in models visible")
        expected = next(x for x in self.rows() if x["id"] == self.builtin)
        actual = self.detail(self.builtin)
        for field in ("checkpoint", "recipe", "downloaded"):
            self.assertEqual(actual[field], expected[field])

    def test_unknown_model_does_not_resolve(self):
        status, _ = self.api.get(
            "/api/v1/models/no-such-compat-model-" + uuid.uuid4().hex
        )
        self.assertEqual(status, 404)

    def test_model_sources_descriptor_contract(self):
        sources = self.assert_ok(self.api.get("/internal/model-sources"))
        self.assertIsInstance(sources.get("catalogs"), list)
        self.assertIsInstance(sources.get("registries"), list)
        keys = [item.get("token") for item in sources["registries"]]
        self.assertIn("huggingface", keys)
        self.assertIn("modelscope", keys)

    def test_downloads_response_gui3_shape(self):
        value = self.assert_ok(self.api.get("/api/v1/downloads"))
        self.assertTrue(
            isinstance(value, list)
            or (isinstance(value, dict) and isinstance(value.get("downloads"), list))
        )

    def test_sync_status_is_a_json_object(self):
        obj = self.assert_ok(self.api.get("/internal/models/sync/status"))
        self.assertIsInstance(obj, dict)

    def test_offline_update_check_rejects_remote_work(self):
        response = self.api.post("/api/v1/models/check-updates", {})
        self.assertEqual(response[0], 409, response[1])

    def test_missing_variant_checkpoint_is_validation_error(self):
        code, _ = self.api.get("/api/v1/pull/variants")
        self.assertEqual(code, 400)

    def test_new_definition_is_visible_without_download(self):
        with self.registered() as (name, public):
            matching = [x for x in self.rows() if x["id"] == public]
            self.assertEqual(len(matching), 1)
            self.assertFalse(matching[0]["downloaded"])
            detail = self.detail(name)
            self.assertEqual(detail["checkpoint"], "lemonade-compat/no-network:Q4_K_M")

    def test_delete_removes_registration_without_touching_other_rows(self):
        before = {x["id"] for x in self.rows()}
        with self.registered() as (name, public):
            self.assertIn(public, {x["id"] for x in self.rows()})
        self.assertEqual(before, {x["id"] for x in self.rows()})

    def test_registered_model_has_model_files_shape(self):
        with self.registered() as (_, public):
            data = self.assert_ok(
                self.api.get("/api/v1/models/" + encoded(public) + "/files")
            )
            self.assertIsInstance(data.get("files"), list)
            self.assertEqual(data.get("model_id"), public)
            for item in data["files"]:
                self.assertIsInstance(item["name"], str)
                self.assertIn("size_bytes", item)
                self.assertIn("exists", item)

    def test_option_round_trip_without_loading(self):
        with self.registered() as (_, public):
            path = "/api/v1/models/" + encoded(public) + "/options"
            baseline = self.assert_ok(self.api.get(path))
            self.assertIsInstance(baseline.get("saved"), dict)
            updated = self.assert_ok(self.api.post(path, {"ctx_size": 8192}))
            self.assertEqual(updated["saved"]["ctx_size"], 8192)
            self.assertEqual(
                self.assert_ok(self.api.get(path))["saved"]["ctx_size"], 8192
            )
            detail = self.detail(public)
            self.assertEqual(detail["recipe_options"]["ctx_size"], 8192)
            self.assert_ok(self.api.delete(path))
            self.assertNotIn("ctx_size", self.assert_ok(self.api.get(path))["saved"])

    def test_options_endpoint_does_not_swallow_model_id(self):
        with self.registered() as (_, public):
            obj = self.assert_ok(
                self.api.get("/api/v1/models/" + encoded(public) + "/options")
            )
            self.assertEqual(obj["model_name"], public)

    def test_files_endpoint_does_not_swallow_model_id(self):
        with self.registered() as (_, public):
            obj = self.assert_ok(
                self.api.get("/v1/models/" + encoded(public) + "/files")
            )
            self.assertIn("files", obj)

    def test_options_reject_unknown_fields_without_partial_save(self):
        with self.registered() as (_, public):
            path = "/api/v1/models/" + encoded(public) + "/options"
            before = self.assert_ok(self.api.get(path))["saved"]
            status, body = self.api.post(
                path, {"ctx_size": 4096, "not_a_real_option": 5}
            )
            self.assertEqual(status, 400, body)
            self.assertEqual(self.assert_ok(self.api.get(path))["saved"], before)

    def test_recipe_options_are_isolated_between_models(self):
        with self.registered() as (_, left):
            with self.registered() as (_, right):
                self.assert_ok(
                    self.api.post(
                        "/api/v1/models/" + encoded(left) + "/options",
                        {"ctx_size": 8192},
                    )
                )
                mine = self.assert_ok(
                    self.api.get("/api/v1/models/" + encoded(left) + "/options")
                )
                other = self.assert_ok(
                    self.api.get("/api/v1/models/" + encoded(right) + "/options")
                )
                self.assertEqual(mine["saved"].get("ctx_size"), 8192)
                self.assertNotIn("ctx_size", other["saved"])

    def test_files_hide_absolute_paths_unless_requested(self):
        with self.registered() as (_, public):
            route = "/api/v1/models/" + encoded(public) + "/files"
            ordinary = self.assert_ok(self.api.get(route))["files"]
            for item in ordinary:
                self.assertNotIn(
                    "path", item, "GUI3 must not receive absolute paths by default"
                )
            explicit = self.assert_ok(self.api.get(route + "?include_paths=true"))[
                "files"
            ]
            self.assertEqual(len(ordinary), len(explicit))

    def test_malformed_registration_json_returns_400(self):
        request = urllib.request.Request(
            self.api.url + "/api/v1/models/register",
            data=b'{"model_name":',
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with self.assertRaises(urllib.error.HTTPError) as raised:
            urllib.request.urlopen(request, timeout=8)
        self.assertEqual(raised.exception.code, 400)
        raised.exception.close()

    def test_conflicting_remote_source_is_rejected_before_download(self):
        response = self.api.post(
            "/api/v1/pull",
            {
                "model_name": self.new_name(),
                "recipe": "llamacpp",
                "stream": False,
                "checkpoint": "https://modelscope.cn/models/org/test-model",
                "source": "huggingface",
                "do_not_upgrade": True,
            },
        )
        self.assertEqual(response[0], 400, response[1])

    def test_multiple_checkpoints_remain_distinct(self):
        with self.registered(
            checkpoints={"main": "org/main:Q4_K_M", "mmproj": "org/vision:mmproj.gguf"},
        ) as (_, public):
            info = self.detail(public)
            self.assertFalse(info["downloaded"])
            self.assertEqual(info["checkpoints"]["main"], "org/main:Q4_K_M")
            self.assertEqual(info["checkpoints"]["mmproj"], "org/vision:mmproj.gguf")
            files = self.assert_ok(
                self.api.get("/api/v1/models/" + encoded(public) + "/files")
            )
            # /files describes resolved physical locations, not every declared
            # checkpoint. A remote mmproj that has never been downloaded may have
            # no resolved path and therefore no /files row.
            self.assertTrue(
                all(item["role"] in info["checkpoints"] for item in files["files"])
            )
            self.assertFalse(
                any(
                    item["role"] == "mmproj" and item["exists"]
                    for item in files["files"]
                )
            )

    def test_cached_main_and_mmproj_files_are_independently_reported(self):
        # A GGUF model's registry checkpoints use repo:variant syntax. Build an
        # offline HF-style snapshot so both files resolve without a download.
        repo_name = "mmproj-fixture-" + uuid.uuid4().hex[:12]
        repo_id = "lemonade-compat/" + repo_name
        repo_cache = self.daemon.models / ("models--lemonade-compat--" + repo_name)
        snapshot = repo_cache / "snapshots" / "fixture"
        main = snapshot / "model-Q4_K_M.gguf"
        mmproj = snapshot / "mmproj-F16.gguf"
        write_gguf(main)
        write_gguf(mmproj)
        (repo_cache / "refs").mkdir(parents=True, exist_ok=True)
        (repo_cache / "refs" / "main").write_text("fixture\n", encoding="utf-8")
        checkpoints = {
            "main": f"{repo_id}:{main.name}",
            "mmproj": f"{repo_id}:{mmproj.name}",
        }

        with self.registered(checkpoints=checkpoints) as (_, public):
            info = self.detail(public)
            self.assertEqual(info["checkpoints"], checkpoints)
            route = "/api/v1/models/" + encoded(public) + "/files"
            body = self.assert_ok(self.api.get(route))
            files = {entry["role"]: entry for entry in body["files"]}
            self.assertEqual(set(files), {"main", "mmproj"}, body)
            for role, path in (("main", main), ("mmproj", mmproj)):
                self.assertEqual(files[role]["name"], path.name)
                self.assertTrue(files[role]["exists"], files[role])
                self.assertEqual(files[role]["size_bytes"], path.stat().st_size)
                self.assertNotIn("path", files[role])

            mmproj.unlink()
            changed = {
                entry["role"]: entry
                for entry in self.assert_ok(self.api.get(route))["files"]
            }
            self.assertTrue(changed["main"]["exists"])
            # A fresh snapshot may omit an unresolved checkpoint; an existing
            # published snapshot may retain its path and report it as missing.
            if "mmproj" in changed:
                self.assertFalse(changed["mmproj"]["exists"])
                self.assertEqual(changed["mmproj"]["size_bytes"], 0)
            self.assertEqual(self.detail(public)["checkpoints"], checkpoints)

    def test_user_registration_shadows_builtin_without_deleting_it(self):
        if not self.builtin:
            self.skipTest("No built-in model for precedence check")
        existing = self.detail(self.builtin)
        canonical = "user." + self.builtin
        checkpoint = "lemonade-compat/shadow-fixture:Q4_K_M"
        registration = {
            "model_name": canonical,
            "recipe": "llamacpp",
            "checkpoint": checkpoint,
            "labels": ["chat"],
        }
        self.assert_ok(self.api.post("/api/v1/models/register", registration))
        try:
            self.assertEqual(self.detail(self.builtin)["checkpoint"], checkpoint)
            shadowed = self.detail("builtin." + self.builtin)
            self.assertEqual(shadowed["checkpoint"], existing["checkpoint"])
        finally:
            self.assert_ok(self.api.post("/api/v1/delete", {"model_name": canonical}))
        self.assertEqual(
            self.detail(self.builtin)["checkpoint"], existing["checkpoint"]
        )

    def test_registry_read_does_not_rewrite_saved_options(self):
        with self.registered() as (_, public):
            path = "/api/v1/models/" + encoded(public) + "/options"
            self.assert_ok(self.api.post(path, {"ctx_size": 8192}))
            for _ in range(20):
                self.rows()
                self.detail(public)
            self.assertEqual(
                self.assert_ok(self.api.get(path))["saved"]["ctx_size"], 8192
            )

    def test_registration_survives_daemon_restart(self):
        with IsolatedLemond(LEMOND) as secondary:
            name = "user.compat-restart-" + uuid.uuid4().hex[:8]
            obj = self.assert_ok(
                secondary.api.post(
                    "/api/v1/models/register",
                    {
                        "model_name": name,
                        "recipe": "llamacpp",
                        "checkpoint": "lemonade-compat/restart:Q4_K_M",
                        "labels": ["chat"],
                    },
                )
            )
            public = obj["model_name"]
            secondary.stop()
            secondary.start()
            data = self.assert_ok(
                secondary.api.get("/api/v1/models/" + encoded(public))
            )
            self.assertEqual(data["checkpoint"], "lemonade-compat/restart:Q4_K_M")

    def test_alias_create_lookup_and_removal(self):
        with self.registered() as (_, public):
            alias = "compat-alias-" + uuid.uuid4().hex[:12]
            body = {"alias": alias, "target": public}
            self.assert_ok(self.api.post("/internal/aliases", body))
            try:
                self.assertEqual(self.detail(alias)["id"], alias)
                aliases = self.assert_ok(self.api.get("/internal/aliases"))["aliases"]
                self.assertTrue(
                    any(
                        x.get("alias") == alias and x.get("target") == public
                        for x in aliases
                    )
                )
            finally:
                self.assert_ok(self.api.delete("/internal/aliases/" + encoded(alias)))
            self.assertEqual(self.api.get("/api/v1/models/" + encoded(alias))[0], 404)

    def test_two_hop_alias_resolves_stably(self):
        with self.registered() as (_, public):
            alias1 = "compat-hop-a-" + uuid.uuid4().hex[:8]
            alias2 = "compat-hop-b-" + uuid.uuid4().hex[:8]
            self.assert_ok(
                self.api.post("/internal/aliases", {"alias": alias1, "target": public})
            )
            try:
                self.assert_ok(
                    self.api.post(
                        "/internal/aliases", {"alias": alias2, "target": alias1}
                    )
                )
                try:
                    self.assertEqual(self.detail(alias2)["id"], alias2)
                finally:
                    self.api.delete("/internal/aliases/" + encoded(alias2))
            finally:
                self.api.delete("/internal/aliases/" + encoded(alias1))

    def test_alias_does_not_hide_real_model(self):
        with self.registered() as (_, public):
            status, _ = self.api.post(
                "/internal/aliases", {"alias": public, "target": "compat-other-target"}
            )
            self.assertEqual(status, 409)
            self.assertNotEqual(
                self.api.get("/api/v1/models/" + encoded(public))[0], 404
            )

    def test_reject_reserved_registration_name(self):
        bad = self.new_name().replace("user.", "user.builtin.", 1)
        status, _ = self.api.post(
            "/api/v1/models/register",
            {"model_name": bad, "recipe": "llamacpp", "checkpoint": "org/repo:Q4_K_M"},
        )
        self.assertEqual(status, 400)
        self.assertEqual(self.api.get("/api/v1/models/" + encoded(bad))[0], 404)

    def test_configured_catalog_live_add_and_remove(self):
        name = "compat-catalog-" + uuid.uuid4().hex[:10]
        catalog_id = "compat-id-" + uuid.uuid4().hex[:10]
        doc = self.daemon.root / f"{catalog_id}.json"
        doc.write_text(
            json.dumps(
                {
                    name: {
                        "recipe": "llamacpp",
                        "labels": ["chat"],
                        "checkpoints": {"main": "org/cfg:Q4_K_M"},
                    }
                }
            ),
            encoding="utf-8",
        )
        try:
            self.assert_ok(
                self.api.post(
                    "/internal/set",
                    {"model_catalogs": [{"id": catalog_id, "path": str(doc)}]},
                )
            )
            self.assertIn(name, {x["id"] for x in self.rows()})
            catalogs = self.assert_ok(self.api.get("/internal/model-sources"))[
                "catalogs"
            ]
            self.assertIn(catalog_id, {x.get("id") for x in catalogs})
        finally:
            self.assert_ok(self.api.post("/internal/set", {"model_catalogs": []}))
        self.assertNotIn(name, {x["id"] for x in self.rows()})

    def test_catalog_rejects_insecure_remote_url_without_network(self):
        code, body = self.api.post(
            "/internal/set",
            {
                "model_catalogs": [
                    {"id": "unsafe-compat", "url": "http://example.invalid/data.json"}
                ]
            },
        )
        self.assertEqual(code, 400, body)
        self.assertNotIn(
            "unsafe-compat",
            {
                x.get("id")
                for x in self.assert_ok(self.api.get("/internal/model-sources"))[
                    "catalogs"
                ]
            },
        )

    def test_extra_models_directory_discovers_new_gguf(self):
        name = "CompatExtra-" + uuid.uuid4().hex[:9]
        path = self.daemon.extra / (name + ".gguf")
        write_gguf(path)
        try:
            # Config mutation invalidates store and prompts an immediate scan.
            self.assert_ok(
                self.api.post(
                    "/internal/set", {"extra_models_dir": str(self.daemon.extra)}
                )
            )
            deadline = time.monotonic() + 8
            rows = []
            while time.monotonic() < deadline:
                rows = [
                    row for row in self.rows() if row.get("checkpoint") == str(path)
                ]
                if rows:
                    break
                time.sleep(0.2)
            self.assertEqual(len(rows), 1, rows)
            self.assertEqual(rows[0]["source"], "extra_models_dir")
            self.assertTrue(rows[0]["downloaded"])
        finally:
            path.unlink(missing_ok=True)
            self.api.post("/internal/set", {"extra_models_dir": ""})

    def test_sharded_extra_model_sees_every_shard(self):
        prefix = "CompatShard-" + uuid.uuid4().hex[:8]
        folder = self.daemon.extra / prefix
        first = folder / (prefix + "-Q4_K_M-00001-of-00002.gguf")
        second = folder / (prefix + "-Q4_K_M-00002-of-00002.gguf")
        for part in (first, second):
            write_gguf(part)
        try:
            self.assert_ok(
                self.api.post(
                    "/internal/set", {"extra_models_dir": str(self.daemon.extra)}
                )
            )
            deadline = time.monotonic() + 8
            entries = []
            while time.monotonic() < deadline:
                entries = [
                    x
                    for x in self.rows()
                    if x.get("checkpoint", "").startswith(str(folder))
                ]
                if entries:
                    break
                time.sleep(0.2)
            self.assertEqual(len(entries), 1, entries)
            self.assertTrue(entries[0]["downloaded"])
        finally:
            shutil.rmtree(folder, ignore_errors=True)
            self.api.post("/internal/set", {"extra_models_dir": ""})

    def test_concurrent_list_reads_during_mutations(self):
        failures = []
        barrier = threading.Event()
        stop = threading.Event()

        def reader():
            barrier.wait(3)
            while not stop.is_set():
                try:
                    status, body = self.api.get("/api/v1/models?show_all=true")
                    if status != 200 or not isinstance(body.get("data"), list):
                        raise AssertionError(f"bad response: {status} {body}")
                    ids = [x["id"] for x in body["data"]]
                    if len(ids) != len(set(ids)):
                        raise AssertionError("duplicate public IDs")
                except Exception as exc:
                    failures.append(repr(exc))
                    stop.set()
                    break

        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            tasks = [pool.submit(reader) for _ in range(5)]
            barrier.set()
            try:
                with self.registered() as (_, public):
                    for size in (4096, 8192, 16384, 2048):
                        result = self.api.post(
                            "/api/v1/models/" + encoded(public) + "/options",
                            {"ctx_size": size},
                        )
                        self.assertEqual(result[0], 200, result[1])
                        self.assertEqual(
                            self.detail(public)["recipe_options"]["ctx_size"], size
                        )
            finally:
                stop.set()
                for future in tasks:
                    future.result(timeout=12)
        self.assertFalse(failures, failures)

    def test_gui3_typescript_client_against_real_isolated_server(self):
        if not CHECK_GUI3:
            self.skipTest("Pass --gui3 to also exercise GUI3's real TypeScript client")
        bridge = HERE / "gui3_bridge.cjs"
        env = {
            key: value
            for key, value in os.environ.items()
            if key not in ("LEMONADE_API_KEY", "LEMONADE_ADMIN_API_KEY")
        }
        env.update(
            {
                "LEMONADE_MODEL_COMPAT_ISOLATED": "1",
                "LEMONADE_MODEL_COMPAT_URL": self.api.url,
            }
        )
        child = subprocess.run(
            ["node", str(bridge)],
            cwd=ROOT,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=90,
        )
        self.assertEqual(child.returncode, 0, child.stdout)
        print(child.stdout.strip())


def main():
    global LEMOND, CHECK_GUI3
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lemond", help="Path to the freshly built lemond binary")
    parser.add_argument(
        "--gui3",
        action="store_true",
        help="Also run GUI3 TS client against this daemon",
    )
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    try:
        LEMOND = find_lemond(args.lemond)
    except FileNotFoundError as error:
        parser.error(str(error))
    CHECK_GUI3 = args.gui3
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ModelManagerApiContract)
    result = unittest.TextTestRunner(verbosity=2 if args.verbose else 1).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
