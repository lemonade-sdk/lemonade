"""Offline edge-case regressions for PR #3828 + GUI3 (no downloads/inference).

Run only against an isolated server using --lemond. See README.md.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import json
from pathlib import Path
import shutil
import sys
import threading
import time
import unittest
import uuid

from advanced_contract import AdvancedModelManagerContract
from server_contract import IsolatedLemond, encoded, find_lemond, write_gguf

BINARY = None


class EdgeModelManagerContract(unittest.TestCase):
    # Share fixture conventions without inheriting the advanced test methods.
    name = AdvancedModelManagerContract.name
    ok = AdvancedModelManagerContract.ok
    detail = AdvancedModelManagerContract.detail
    all_rows = AdvancedModelManagerContract.all_rows
    files = AdvancedModelManagerContract.files
    registered = AdvancedModelManagerContract.registered
    repository = AdvancedModelManagerContract.repository
    _rebuild = AdvancedModelManagerContract._rebuild

    @classmethod
    def setUpClass(cls):
        cls.daemon = IsolatedLemond(BINARY)
        cls.daemon.start()
        cls.api = cls.daemon.api

    @classmethod
    def tearDownClass(cls):
        cls.daemon.close()

    def _model(self, name):
        return self.api.get("/api/v1/models/" + encoded(name))

    def _invalidate(self):
        with self.registered():
            pass

    def _setup_extra(self, paths):
        for path in paths:
            write_gguf(path)
        self.ok(
            self.api.post("/internal/set", {"extra_models_dir": str(self.daemon.extra)})
        )

    def _clear_extra(self, paths):
        self.ok(self.api.post("/internal/set", {"extra_models_dir": ""}))
        for path in paths:
            path.unlink(missing_ok=True)
            parent = path.parent
            while parent != self.daemon.extra and parent.is_dir():
                try:
                    parent.rmdir()
                except OSError:
                    break
                parent = parent.parent

    def test_uncommitted_snapshot_without_ref_is_not_downloaded(self):
        with self.repository(["model-Q4_K_M.gguf"]) as (repo, cache, _):
            (cache / "refs" / "main").unlink()
            with self.registered(checkpoint=repo + ":model-Q4_K_M.gguf") as (_, public):
                self.assertFalse(self.detail(public)["downloaded"])

    def test_dangling_active_snapshot_ref_is_not_downloaded(self):
        with self.repository(["model-Q4_K_M.gguf"]) as (repo, cache, _):
            (cache / "refs" / "main").write_text("does-not-exist\n", encoding="utf-8")
            with self.registered(checkpoint=repo + ":model-Q4_K_M.gguf") as (_, public):
                self.assertFalse(self.detail(public)["downloaded"])

    def test_restoring_active_snapshot_ref_recovers_downloaded_state(self):
        with self.repository(["model-Q4_K_M.gguf"]) as (repo, cache, _):
            ref = cache / "refs" / "main"
            ref.unlink()
            with self.registered(checkpoint=repo + ":model-Q4_K_M.gguf") as (_, public):
                self.assertFalse(self.detail(public)["downloaded"])
                ref.write_text("fixture\n", encoding="utf-8")
                self._invalidate()
                self.assertTrue(self.detail(public)["downloaded"])

    def test_old_complete_variant_survives_new_snapshot_activation(self):
        with self.repository(["model-Q4_K_M.gguf"], revision="old") as (
            repo,
            cache,
            old,
        ):
            new = cache / "snapshots" / "new"
            write_gguf(new / "model-Q8_0.gguf")
            (cache / "refs" / "main").write_text("new\n", encoding="utf-8")
            with self.registered(checkpoint=repo + ":model-Q4_K_M.gguf") as (_, public):
                self.assertTrue(self.detail(public)["downloaded"])
                found = {f["role"]: f for f in self.files(public, paths=True)}
                self.assertEqual(Path(found["main"]["path"]), old / "model-Q4_K_M.gguf")

    def test_uncommitted_new_snapshot_does_not_hide_old_variant(self):
        with self.repository(["model-Q4_K_M.gguf"], revision="old") as (
            repo,
            cache,
            old,
        ):
            incomplete = cache / "snapshots" / "unfinished"
            write_gguf(incomplete / "model-Q8_0.gguf")
            (incomplete / ".download_manifest.json").write_text(
                json.dumps({"files": ["model-Q8_0.gguf"]}), encoding="utf-8"
            )
            (cache / "refs" / "main").write_text("unfinished\n", encoding="utf-8")
            with self.registered(checkpoint=repo + ":model-Q4_K_M.gguf") as (_, public):
                self.assertTrue(self.detail(public)["downloaded"])
                found = {f["role"]: f for f in self.files(public, paths=True)}
                self.assertEqual(Path(found["main"]["path"]), old / "model-Q4_K_M.gguf")

    def test_corrupted_file_can_recover_after_cache_rebuild(self):
        with self.repository(["model-Q4_K_M.gguf"]) as (repo, _, snapshot):
            model_file = snapshot / "model-Q4_K_M.gguf"
            model_file.write_bytes(b"bad")
            with self.registered(checkpoint=repo + ":model-Q4_K_M.gguf") as (_, public):
                self.assertFalse(self.detail(public)["downloaded"])
                write_gguf(model_file)
                self._invalidate()
                self.assertTrue(self.detail(public)["downloaded"])
                model_file.write_bytes(b"bad again")
                self._invalidate()
                self.assertFalse(self.detail(public)["downloaded"])

    def test_extra_directory_ignores_draft_and_auxiliary_only_ggufs(self):
        prefix = "edge-draft-" + uuid.uuid4().hex[:9]
        accepted = self.daemon.extra / (prefix + "-Q4_K_M.gguf")
        ignored = [
            self.daemon.extra / ("mtp-" + prefix + ".gguf"),
            self.daemon.extra / ("dflash-" + prefix + ".gguf"),
            self.daemon.extra / ("mmproj-" + prefix + ".gguf"),
        ]
        files = [accepted, *ignored]
        try:
            self._setup_extra(files)
            seen = {
                row.get("checkpoint")
                for row in self.all_rows()
                if row.get("source") == "extra_models_dir"
            }
            self.assertIn(str(accepted), seen)
            self.assertTrue(set(map(str, ignored)).isdisjoint(seen), seen)
        finally:
            self._clear_extra(files)

    def test_extra_directory_category_labels_are_preserved(self):
        token = uuid.uuid4().hex[:8]
        paths = {
            label: self.daemon.extra / label / (token + "-Q4_K_M.gguf")
            for label in ("chat", "embeddings", "reranking")
        }
        try:
            self._setup_extra(paths.values())
            visible = {
                row.get("checkpoint"): row
                for row in self.all_rows()
                if row.get("source") == "extra_models_dir"
            }
            for label, file in paths.items():
                self.assertIn(str(file), visible, visible)
                self.assertIn(label, visible[str(file)].get("labels", []))
        finally:
            self._clear_extra(paths.values())

    def test_extra_file_disappears_after_explicit_directory_reset(self):
        extra = self.daemon.extra / ("edge-disappear-" + uuid.uuid4().hex[:8] + ".gguf")
        try:
            self._setup_extra([extra])
            rows = {row.get("checkpoint") for row in self.all_rows()}
            self.assertIn(str(extra), rows)
            extra.unlink()
            self.ok(self.api.post("/internal/set", {"extra_models_dir": ""}))
            self.ok(
                self.api.post(
                    "/internal/set", {"extra_models_dir": str(self.daemon.extra)}
                )
            )
            self.assertNotIn(
                str(extra), {row.get("checkpoint") for row in self.all_rows()}
            )
        finally:
            self._clear_extra([extra])

    def test_cyclic_aliases_loaded_from_disk_are_sanitized(self):
        with IsolatedLemond(BINARY) as daemon:
            first, second = ("edge-cycle-" + uuid.uuid4().hex[:8] for _ in range(2))
            daemon.stop()
            (daemon.cache / "aliases.json").write_text(
                json.dumps({first: second, second: first}), encoding="utf-8"
            )
            daemon.start()
            rows = self.ok(daemon.api.get("/internal/aliases"))["aliases"]
            self.assertFalse({first, second} & {row["alias"] for row in rows}, rows)

    def test_invalid_alias_json_is_not_applied_on_restart(self):
        with IsolatedLemond(BINARY) as daemon:
            daemon.stop()
            (daemon.cache / "aliases.json").write_text("{ broken", encoding="utf-8")
            daemon.start()
            self.assertEqual(
                self.ok(daemon.api.get("/internal/aliases"))["aliases"], []
            )
            self.assertTrue((daemon.cache / "aliases.json.corrupted").is_file())

    def test_invalid_source_does_not_modify_existing_registration(self):
        with self.registered(checkpoint="lemonade-compat/stable:Q4_K_M") as (_, public):
            before = self.detail(public)["checkpoint"]
            name = self.name()
            code, result = self.api.post(
                "/api/v1/models/register",
                {
                    "model_name": name,
                    "recipe": "llamacpp",
                    "labels": ["chat"],
                    "checkpoint": "lemonade-compat/invalid:Q4_K_M",
                    "source": "not-a-registry",
                },
            )
            self.assertEqual(code, 400, result)
            self.assertEqual(self._model(name)[0], 404)
            self.assertEqual(self.detail(public)["checkpoint"], before)

    def test_idempotent_registration_preserves_single_public_row(self):
        with self.registered(checkpoint="lemonade-compat/idempotent:Q4_K_M") as (
            name,
            public,
        ):
            payload = {
                "model_name": name,
                "recipe": "llamacpp",
                "labels": ["chat"],
                "checkpoint": "lemonade-compat/idempotent:Q4_K_M",
            }
            for _ in range(8):
                self.assertEqual(
                    self.ok(self.api.post("/api/v1/models/register", payload))[
                        "model_name"
                    ],
                    public,
                )
                self.assertEqual(sum(row["id"] == public for row in self.all_rows()), 1)

    def test_reader_storm_during_distinct_registrations_and_deletions(self):
        baseline = next(
            row["id"]
            for row in self.all_rows()
            if not row["id"].startswith(("user.", "extra."))
        )
        names = [self.name() for _ in range(12)]
        start = threading.Event()

        def writer(name):
            start.wait(timeout=5)
            response = self.api.post(
                "/api/v1/models/register",
                {
                    "model_name": name,
                    "recipe": "llamacpp",
                    "labels": ["chat"],
                    "checkpoint": "lemonade-compat/reader-storm:Q4_K_M",
                },
            )
            if response[0] != 200:
                return ("register", name, response)
            model = self._model(name)
            if model[0] != 200:
                return ("detail", name, model)
            return ("ok", name, response)

        def reader():
            start.wait(timeout=5)
            bad = []
            for _ in range(30):
                for route in (
                    "/api/v1/models?show_all=true",
                    "/api/v1/models/" + encoded(baseline),
                ):
                    code, body = self.api.get(route)
                    if code != 200:
                        bad.append((route, code, body))
            return bad

        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
                workers = [pool.submit(writer, name) for name in names]
                readers = [pool.submit(reader) for _ in range(4)]
                start.set()
                results = [job.result(timeout=45) for job in workers]
                failures = [
                    failure for task in readers for failure in task.result(timeout=45)
                ]
            self.assertFalse(failures, failures[:8])
            self.assertTrue(all(result[0] == "ok" for result in results), results)
            ids = {row["id"] for row in self.all_rows()}
            self.assertTrue(all(self._model(name)[0] == 200 for name in names), ids)
        finally:
            with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
                list(
                    pool.map(
                        lambda name: self.api.post(
                            "/api/v1/delete", {"model_name": name}
                        ),
                        names,
                    )
                )

    def test_parallel_definition_replacement_and_reads(self):
        with contextlib.ExitStack() as stack:
            models = [stack.enter_context(self.registered())[0] for _ in range(6)]

            def replace(item):
                position, name = item
                checkpoint = f"lemonade-compat/replace-{position}:Q8_0"
                response = self.api.post(
                    "/api/v1/models/register",
                    {
                        "model_name": name,
                        "recipe": "llamacpp",
                        "labels": ["chat"],
                        "checkpoint": checkpoint,
                    },
                )
                return (name, checkpoint, response)

            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                pending = [pool.submit(replace, item) for item in enumerate(models)]
                pending += [pool.submit(self.all_rows) for _ in range(4)]
                results = [item.result(timeout=30) for item in pending[: len(models)]]
                for item in pending[len(models) :]:
                    item.result(timeout=30)
            for name, checkpoint, response in results:
                self.ok(response)
                self.assertEqual(self.detail(name)["checkpoint"], checkpoint)

    def test_catalog_change_does_not_break_concurrent_model_lists(self):
        catalog_id = "edge-cat-" + uuid.uuid4().hex[:10]
        model_id = "edge-cat-model-" + uuid.uuid4().hex[:10]
        file = self.daemon.root / (catalog_id + ".json")
        file.write_text(
            json.dumps(
                {
                    model_id: {
                        "recipe": "llamacpp",
                        "labels": ["chat"],
                        "checkpoint": "lemonade-compat/catalog-toggle:Q4_K_M",
                    }
                }
            ),
            encoding="utf-8",
        )
        start = threading.Event()

        def writer():
            start.wait(timeout=5)
            for _ in range(5):
                code, result = self.api.post(
                    "/internal/set",
                    {"model_catalogs": [{"id": catalog_id, "path": str(file)}]},
                )
                if code != 200:
                    return (code, result)
                code, result = self.api.post("/internal/set", {"model_catalogs": []})
                if code != 200:
                    return (code, result)
            return (200, {})

        def reader():
            start.wait(timeout=5)
            return [
                response
                for _ in range(35)
                if (response := self.api.get("/api/v1/models?show_all=true"))[0] != 200
            ]

        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
                write_job = pool.submit(writer)
                read_jobs = [pool.submit(reader) for _ in range(3)]
                start.set()
                self.ok(write_job.result(timeout=40))
                failures = [row for job in read_jobs for row in job.result(timeout=40)]
            self.assertFalse(failures, failures[:5])
            self.assertNotIn(model_id, {row["id"] for row in self.all_rows()})
        finally:
            self.api.post("/internal/set", {"model_catalogs": []})
            file.unlink(missing_ok=True)

    def test_register_delete_churn_never_reuses_stale_identity(self):
        for i in range(12):
            name = self.name()
            checkpoint = f"lemonade-compat/churn-{i}:Q4_K_M"
            registered = self.ok(
                self.api.post(
                    "/api/v1/models/register",
                    {
                        "model_name": name,
                        "recipe": "llamacpp",
                        "labels": ["chat"],
                        "checkpoint": checkpoint,
                    },
                )
            )
            self.assertEqual(
                self.detail(registered["model_name"])["checkpoint"], checkpoint
            )
            self.ok(self.api.post("/api/v1/delete", {"model_name": name}))
            self.assertEqual(self._model(name)[0], 404)


def main():
    global BINARY
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lemond", help="Built lemond from PR #3828 + GUI3")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--match", help="Run only tests containing this substring")
    args = parser.parse_args()
    try:
        BINARY = find_lemond(args.lemond)
    except FileNotFoundError as error:
        parser.error(str(error))
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(EdgeModelManagerContract)
    if args.match:
        suite = unittest.TestSuite(test for test in suite if args.match in test.id())
    result = unittest.TextTestRunner(verbosity=2 if args.verbose else 1).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
