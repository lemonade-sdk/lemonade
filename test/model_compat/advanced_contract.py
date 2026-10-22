"""High-value, offline model-manager integration tests against a PRIVATE lemond.

Run explicitly after building lemond; never targets an existing Lemonade server.
Only the Python standard library is needed. See ADVANCED.md.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import json
from pathlib import Path
import shutil
import sys
import time
import unittest
import uuid

from server_contract import IsolatedLemond, encoded, find_lemond, write_gguf

BINARY = None


class AdvancedModelManagerContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.daemon = IsolatedLemond(BINARY)
        cls.daemon.start()
        cls.api = cls.daemon.api

    @classmethod
    def tearDownClass(cls):
        cls.daemon.close()

    def name(self):
        return "user.deep-" + uuid.uuid4().hex[:12]

    def ok(self, response):
        code, body = response
        self.assertEqual(code, 200, body)
        return body

    def detail(self, model):
        return self.ok(self.api.get("/api/v1/models/" + encoded(model)))

    def all_rows(self):
        return self.ok(self.api.get("/api/v1/models?show_all=true"))["data"]

    def files(self, model, *, paths=False):
        route = "/api/v1/models/" + encoded(model) + "/files"
        if paths:
            route += "?include_paths=true"
        return self.ok(self.api.get(route))["files"]

    @contextlib.contextmanager
    def registered(self, *, checkpoint=None, checkpoints=None, source=None, **fields):
        name = self.name()
        body = {"model_name": name, "recipe": "llamacpp", "labels": ["chat"], **fields}
        if checkpoints is not None:
            body["checkpoints"] = checkpoints
        else:
            body["checkpoint"] = checkpoint or "lemonade-compat/advanced:Q4_K_M"
        if source is not None:
            body["source"] = source
        response = self.ok(self.api.post("/api/v1/models/register", body))
        public = response["model_name"]
        try:
            yield name, public
        finally:
            code, result = self.api.post("/api/v1/delete", {"model_name": name})
            self.assertIn(code, (200, 404), f"Cleanup of {name}: {code} {result}")

    @contextlib.contextmanager
    def repository(
        self, files, *, source="huggingface", revision="fixture", repo_name=None
    ):
        repo_name = repo_name or "deep-" + uuid.uuid4().hex[:12]
        repo_id = "lemonade-compat/" + repo_name
        prefix = "models--" if source == "huggingface" else "modelscope--models--"
        cache = self.daemon.models / (prefix + "lemonade-compat--" + repo_name)
        snapshot = cache / "snapshots" / revision
        snapshot.mkdir(parents=True)
        for filename in files:
            write_gguf(snapshot / filename)
        (cache / "refs").mkdir()
        (cache / "refs" / "main").write_text(revision + "\n", encoding="utf-8")
        try:
            yield repo_id, cache, snapshot
        finally:
            shutil.rmtree(cache, ignore_errors=True)

    def _rebuild(self):
        # Registration and deletion both invalidate the published model snapshot.
        # No private C++ methods, sleeps, or timing-sensitive watcher events.
        with self.registered():
            pass

    def test_corrupt_gguf_exists_but_is_not_downloaded(self):
        with self.repository(["model-Q4_K_M.gguf"]) as (repo, _, snap):
            (snap / "model-Q4_K_M.gguf").write_bytes(b"not a GGUF")
            with self.registered(checkpoint=repo + ":model-Q4_K_M.gguf") as (_, public):
                self.assertFalse(self.detail(public)["downloaded"])
                entries = {f["role"]: f for f in self.files(public)}
                self.assertTrue(entries["main"]["exists"])
                self.assertNotIn("path", entries["main"])

    def test_partial_marker_blocks_downloaded_state(self):
        with self.repository(["model-Q4_K_M.gguf"]) as (repo, _, snap):
            (snap / "model-Q4_K_M.gguf.partial").write_bytes(b"unfinished")
            with self.registered(checkpoint=repo + ":model-Q4_K_M.gguf") as (_, public):
                self.assertFalse(self.detail(public)["downloaded"])

    def test_download_manifest_blocks_downloaded_state(self):
        with self.repository(["model-Q4_K_M.gguf"]) as (repo, _, snap):
            (snap / ".download_manifest.json").write_text(
                json.dumps({"files": ["model-Q4_K_M.gguf"]}), encoding="utf-8"
            )
            with self.registered(checkpoint=repo + ":model-Q4_K_M.gguf") as (_, public):
                self.assertFalse(self.detail(public)["downloaded"])

    def test_other_quantization_is_not_mistaken_for_requested_variant(self):
        with self.repository(["model-Q8_0.gguf"]) as (repo, _, _):
            with self.registered(checkpoint=repo + ":Q4_K_M") as (_, public):
                self.assertFalse(self.detail(public)["downloaded"])
                self.assertNotEqual(self.detail(public)["checkpoint"], repo + ":Q8_0")

    def test_main_and_mmproj_in_different_repositories(self):
        with self.repository(["model-Q4_K_M.gguf"]) as (main_repo, _, main_snap):
            with self.repository(["mmproj-F16.gguf"]) as (aux_repo, _, aux_snap):
                checkpoints = {
                    "main": main_repo + ":model-Q4_K_M.gguf",
                    "mmproj": aux_repo + ":mmproj-F16.gguf",
                }
                with self.registered(checkpoints=checkpoints) as (_, public):
                    self.assertEqual(self.detail(public)["checkpoints"], checkpoints)
                    found = {f["role"]: f for f in self.files(public, paths=True)}
                    self.assertEqual(set(found), {"main", "mmproj"})
                    self.assertEqual(
                        Path(found["main"]["path"]), main_snap / "model-Q4_K_M.gguf"
                    )
                    self.assertEqual(
                        Path(found["mmproj"]["path"]), aux_snap / "mmproj-F16.gguf"
                    )
                    self.assertTrue(all(f["exists"] for f in found.values()))
                    self.assertTrue(self.detail(public)["downloaded"])

    def test_legacy_mmproj_field_keeps_its_companion_role(self):
        with self.repository(["model-Q4_K_M.gguf", "mmproj-F16.gguf"]) as (
            repo,
            _,
            snap,
        ):
            with self.registered(
                checkpoint=repo + ":model-Q4_K_M.gguf", mmproj="mmproj-F16.gguf"
            ) as (_, public):
                detail = self.detail(public)
                self.assertEqual(
                    detail["checkpoints"]["mmproj"], repo + ":mmproj-F16.gguf"
                )
                entries = {f["role"]: f for f in self.files(public, paths=True)}
                self.assertEqual(
                    Path(entries["mmproj"]["path"]), snap / "mmproj-F16.gguf"
                )
                self.assertTrue(entries["mmproj"]["exists"])

    def test_active_snapshot_change_is_seen_after_store_invalidation(self):
        with self.repository(["model-Q4_K_M.gguf"], revision="rev-a") as (
            repo,
            cache,
            a,
        ):
            b = cache / "snapshots" / "rev-b"
            write_gguf(b / "model-Q4_K_M.gguf")
            checkpoint = repo + ":model-Q4_K_M.gguf"
            with self.registered(checkpoint=checkpoint) as (_, public):
                before = {f["role"]: f for f in self.files(public, paths=True)}
                self.assertEqual(Path(before["main"]["path"]), a / "model-Q4_K_M.gguf")
                (cache / "refs" / "main").write_text("rev-b\n", encoding="utf-8")
                self._rebuild()
                after = {f["role"]: f for f in self.files(public, paths=True)}
                self.assertEqual(Path(after["main"]["path"]), b / "model-Q4_K_M.gguf")
                self.assertEqual(self.detail(public)["checkpoint"], checkpoint)

    def test_deleting_one_quantization_preserves_another(self):
        first_file, second_file = "model-Q4_K_M.gguf", "model-Q8_0.gguf"
        with self.repository([first_file, second_file]) as (repo, _, snap):
            with self.registered(checkpoint=repo + ":" + second_file) as (_, survivor):
                with self.registered(checkpoint=repo + ":" + first_file) as (_, victim):
                    self.assertTrue(self.detail(survivor)["downloaded"])
                    self.assertTrue(self.detail(victim)["downloaded"])
                self.assertTrue((snap / second_file).is_file())
                self.assertTrue(self.detail(survivor)["downloaded"])
                self.assertTrue(
                    {f["role"]: f for f in self.files(survivor)}["main"]["exists"]
                )

    def test_deleting_one_model_preserves_shared_mmproj(self):
        files = ["model-Q4_K_M.gguf", "model-Q8_0.gguf", "mmproj-F16.gguf"]
        with self.repository(files) as (repo, _, snap):
            mmproj_cp = repo + ":mmproj-F16.gguf"
            survivor_cp = {"main": repo + ":model-Q8_0.gguf", "mmproj": mmproj_cp}
            victim_cp = {"main": repo + ":model-Q4_K_M.gguf", "mmproj": mmproj_cp}
            with self.registered(checkpoints=survivor_cp) as (_, survivor):
                with self.registered(checkpoints=victim_cp) as (_, victim):
                    self.assertTrue(self.detail(victim)["downloaded"])
                self.assertTrue((snap / "mmproj-F16.gguf").is_file())
                self.assertTrue(self.detail(survivor)["downloaded"])
                self.assertTrue(
                    {f["role"]: f for f in self.files(survivor)}["mmproj"]["exists"]
                )

    def test_deleting_external_local_path_never_removes_user_file(self):
        external = self.daemon.root / ("external-" + uuid.uuid4().hex + ".bin")
        external.write_bytes(b"private user bytes")
        try:
            with self.registered(checkpoint=str(external), source="local_path") as (
                _,
                public,
            ):
                self.assertEqual(self.detail(public)["checkpoint"], str(external))
            self.assertEqual(external.read_bytes(), b"private user bytes")
        finally:
            external.unlink(missing_ok=True)

    def test_deleting_symlinked_cache_entry_never_removes_outside_target(self):
        # A shared cache may contain an HF-style symlink. Deleting a model must
        # never delete the symlink's target outside the managed model directory.
        with self.repository([]) as (repo, _, snap):
            target = self.daemon.root / ("symlink-target-" + uuid.uuid4().hex + ".gguf")
            write_gguf(target)
            (snap / "model-Q4_K_M.gguf").symlink_to(target)
            try:
                with self.registered(checkpoint=repo + ":model-Q4_K_M.gguf"):
                    pass
                self.assertEqual(target.read_bytes()[:4], b"GGUF")
            finally:
                target.unlink(missing_ok=True)

    def test_registry_source_namespaces_do_not_share_disk_cache(self):
        filename = "model-Q4_K_M.gguf"
        # Two providers expose the same repository identity, not the same data.
        with self.repository([filename], source="huggingface") as (
            hf_repo,
            hf_cache,
            _,
        ):
            with self.repository(
                [filename], source="modelscope", repo_name=hf_repo.split("/", 1)[1]
            ) as (ms_repo, ms_cache, _):
                self.assertEqual(hf_repo, ms_repo)
                with self.registered(
                    checkpoint=hf_repo + ":" + filename, source="huggingface"
                ) as (_, hf):
                    with self.registered(
                        checkpoint=ms_repo + ":" + filename, source="modelscope"
                    ) as (_, ms):
                        hf_path = {f["role"]: f for f in self.files(hf, paths=True)}[
                            "main"
                        ]["path"]
                        ms_path = {f["role"]: f for f in self.files(ms, paths=True)}[
                            "main"
                        ]["path"]
                        self.assertNotEqual(hf_path, ms_path)
                        self.assertTrue(Path(hf_path).is_relative_to(hf_cache))
                        self.assertTrue(Path(ms_path).is_relative_to(ms_cache))
                    self.assertTrue(self.detail(hf)["downloaded"])
                    self.assertTrue(Path(hf_path).is_file())

    def test_parallel_registrations_do_not_lose_updates(self):
        names = [self.name() for _ in range(10)]
        public = {}
        try:

            def add(name):
                return self.api.post(
                    "/api/v1/models/register",
                    {
                        "model_name": name,
                        "recipe": "llamacpp",
                        "labels": ["chat"],
                        "checkpoint": "lemonade-compat/parallel:Q4_K_M",
                    },
                )

            with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
                results = list(pool.map(add, names))
            for name, response in zip(names, results):
                if response[0] != 200:
                    status, detail = self.api.get("/api/v1/models/" + encoded(name))
                    checkpoint = (
                        detail.get("checkpoint") if isinstance(detail, dict) else None
                    )
                    self.fail(
                        f"{name}: register returned {response!r}; "
                        f"subsequent lookup: HTTP {status}, checkpoint={checkpoint!r}"
                    )
                public[name] = response[1]["model_name"]
            listed = {item["id"] for item in self.all_rows()}
            self.assertTrue(set(public.values()).issubset(listed))
            for name, public_name in public.items():
                self.assertEqual(
                    self.detail(public_name)["checkpoint"],
                    "lemonade-compat/parallel:Q4_K_M",
                    name,
                )
        finally:
            with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
                cleanups = list(
                    pool.map(
                        lambda name: self.api.post(
                            "/api/v1/delete", {"model_name": name}
                        ),
                        names,
                    )
                )
            for response in cleanups:
                self.assertIn(response[0], (200, 404, 422), response)

    def test_parallel_option_writes_preserve_independent_models(self):
        with contextlib.ExitStack() as stack:
            public = [stack.enter_context(self.registered())[1] for _ in range(6)]
            chosen = {name: 4096 * (i + 1) for i, name in enumerate(public)}

            def save(item):
                name, ctx_size = item
                return self.api.post(
                    "/api/v1/models/" + encoded(name) + "/options",
                    {"ctx_size": ctx_size},
                )

            with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
                results = list(pool.map(save, chosen.items()))
            for response in results:
                self.ok(response)
            for name, ctx_size in chosen.items():
                options = self.ok(
                    self.api.get("/api/v1/models/" + encoded(name) + "/options")
                )
                self.assertEqual(options["saved"]["ctx_size"], ctx_size)

    def test_replacing_registration_does_not_duplicate_or_leave_old_checkpoint(self):
        with self.registered(checkpoint="lemonade-compat/old:Q4_K_M") as (name, public):
            replaced = self.ok(
                self.api.post(
                    "/api/v1/models/register",
                    {
                        "model_name": name,
                        "recipe": "llamacpp",
                        "labels": ["chat"],
                        "checkpoint": "lemonade-compat/new:Q8_0",
                    },
                )
            )
            self.assertEqual(replaced["model_name"], public)
            self.assertEqual(
                self.detail(public)["checkpoint"], "lemonade-compat/new:Q8_0"
            )
            self.assertEqual(sum(item["id"] == public for item in self.all_rows()), 1)

    def test_invalid_multi_checkpoint_registration_has_no_side_effect(self):
        name = self.name()
        code, result = self.api.post(
            "/api/v1/models/register",
            {
                "model_name": name,
                "recipe": "llamacpp",
                "checkpoints": {"mmproj": "lemonade-compat/aux:mmproj.gguf"},
            },
        )
        self.assertEqual(code, 400, result)
        self.assertEqual(self.api.get("/api/v1/models/" + encoded(name))[0], 404)

    def test_alias_cycle_is_rejected_without_mutating_existing_targets(self):
        with self.registered() as (_, public):
            first = "deep-a-" + uuid.uuid4().hex[:8]
            second = "deep-b-" + uuid.uuid4().hex[:8]
            self.ok(
                self.api.post("/internal/aliases", {"alias": first, "target": public})
            )
            try:
                self.ok(
                    self.api.post(
                        "/internal/aliases", {"alias": second, "target": first}
                    )
                )
                try:
                    code, result = self.api.post(
                        "/internal/aliases", {"alias": first, "target": second}
                    )
                    self.assertEqual(code, 409, result)
                    bindings = {
                        x["alias"]: x["target"]
                        for x in self.ok(self.api.get("/internal/aliases"))["aliases"]
                    }
                    self.assertEqual(bindings[first], public)
                    self.assertEqual(bindings[second], first)
                    self.assertEqual(self.detail(second)["id"], second)
                finally:
                    self.ok(self.api.delete("/internal/aliases/" + encoded(second)))
            finally:
                self.ok(self.api.delete("/internal/aliases/" + encoded(first)))

    def test_alias_and_options_survive_restart(self):
        with IsolatedLemond(BINARY) as secondary:
            name = self.name()
            public = self.ok(
                secondary.api.post(
                    "/api/v1/models/register",
                    {
                        "model_name": name,
                        "recipe": "llamacpp",
                        "labels": ["chat"],
                        "checkpoint": "lemonade-compat/restart-deep:Q4_K_M",
                    },
                )
            )["model_name"]
            option_route = "/api/v1/models/" + encoded(public) + "/options"
            self.ok(secondary.api.post(option_route, {"ctx_size": 12288}))
            alias = "deep-restart-" + uuid.uuid4().hex[:9]
            self.ok(
                secondary.api.post(
                    "/internal/aliases", {"alias": alias, "target": public}
                )
            )
            secondary.stop()
            secondary.start()
            self.assertEqual(
                self.ok(secondary.api.get(option_route))["saved"]["ctx_size"], 12288
            )
            self.assertEqual(
                self.ok(secondary.api.get("/api/v1/models/" + encoded(alias)))["id"],
                alias,
            )
            self.ok(secondary.api.delete(option_route))
            secondary.stop()
            secondary.start()
            self.assertNotIn(
                "ctx_size", self.ok(secondary.api.get(option_route))["saved"]
            )

    def test_valid_configured_catalog_survives_invalid_neighbor(self):
        identifier = "deep-cat-" + uuid.uuid4().hex[:8]
        model_name = "deep-visible-" + uuid.uuid4().hex[:8]
        good_file = self.daemon.root / (identifier + ".json")
        bad_file = self.daemon.root / (identifier + "-bad.json")
        good_file.write_text(
            json.dumps(
                {
                    model_name: {
                        "recipe": "llamacpp",
                        "checkpoint": "lemonade-compat/catalog:Q4_K_M",
                        "labels": ["chat"],
                    }
                }
            ),
            encoding="utf-8",
        )
        bad_file.write_text("{invalid json", encoding="utf-8")
        try:
            self.ok(
                self.api.post(
                    "/internal/set",
                    {
                        "model_catalogs": [
                            {"id": identifier, "path": str(good_file)},
                            {"id": identifier + "-bad", "path": str(bad_file)},
                        ]
                    },
                )
            )
            self.assertIn(model_name, {entry["id"] for entry in self.all_rows()})
        finally:
            self.ok(self.api.post("/internal/set", {"model_catalogs": []}))
            good_file.unlink(missing_ok=True)
            bad_file.unlink(missing_ok=True)

    def test_extra_directory_duplicate_filenames_get_distinct_public_ids(self):
        prefix = "deep-extra-" + uuid.uuid4().hex[:10]
        folder_a = self.daemon.extra / (prefix + "-a")
        folder_b = self.daemon.extra / (prefix + "-b")
        main_a = folder_a / "same-Q4_K_M.gguf"
        main_b = folder_b / "same-Q4_K_M.gguf"
        write_gguf(main_a)
        write_gguf(main_b)
        try:
            self.ok(
                self.api.post(
                    "/internal/set", {"extra_models_dir": str(self.daemon.extra)}
                )
            )
            records = []
            deadline = time.monotonic() + 6
            while time.monotonic() < deadline:
                records = [
                    row
                    for row in self.all_rows()
                    if row.get("source") == "extra_models_dir"
                    and row.get("checkpoint")
                    in (str(folder_a), str(folder_b), str(main_a), str(main_b))
                ]
                if len(records) == 2:
                    break
                time.sleep(0.15)
            self.assertEqual(len(records), 2, records)
            self.assertEqual(len({row["id"] for row in records}), 2)
        finally:
            self.ok(self.api.post("/internal/set", {"extra_models_dir": ""}))
            shutil.rmtree(folder_a, ignore_errors=True)
            shutil.rmtree(folder_b, ignore_errors=True)


def main():
    global BINARY
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lemond", help="Built lemond from the merge branch")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--match", help="Run only test names containing this string")
    args = parser.parse_args()
    try:
        BINARY = find_lemond(args.lemond)
    except FileNotFoundError as error:
        parser.error(str(error))
    tests = unittest.defaultTestLoader.loadTestsFromTestCase(
        AdvancedModelManagerContract
    )
    if args.match:
        tests = unittest.TestSuite(test for test in tests if args.match in test.id())
    result = unittest.TextTestRunner(verbosity=2 if args.verbose else 1).run(tests)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
