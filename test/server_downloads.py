"""
Server-owned download registry tests for Lemonade Server.

Fast registry-only checks run by default and do not download real model files.
The hash/corruption smoke checks are opt-in for local runs and enabled by the
compact CI workflow via LEMONADE_RUN_REAL_DOWNLOAD_TESTS=1.

Usage:
    python test/server_downloads.py
    python test/server_downloads.py --cli-binary /path/to/lemonade

Real download/SHA corruption smoke test:
    LEMONADE_RUN_REAL_DOWNLOAD_TESTS=1 \
    LEMONADE_DOWNLOAD_TEST_MODEL=Tiny-Test-Model-GGUF \
    python test/server_downloads.py --cli-binary ./build/lemonade
"""

from __future__ import annotations

import hashlib
import os
import time
import uuid
from pathlib import Path
from urllib.parse import quote

import requests

from utils.server_base import ServerTestBase, run_server_tests
from utils.test_models import (
    ENDPOINT_TEST_MODEL,
    PORT,
    TIMEOUT_DEFAULT,
    get_hf_cache_dir_candidates,
)


DEFAULT_POLL_SECONDS = float(os.environ.get("LEMONADE_DOWNLOAD_TEST_POLL_SECONDS", "0.25"))
DEFAULT_WAIT_SECONDS = float(os.environ.get("LEMONADE_DOWNLOAD_TEST_WAIT_SECONDS", "25"))
REAL_DOWNLOAD_WAIT_SECONDS = float(
    os.environ.get("LEMONADE_DOWNLOAD_REAL_TEST_WAIT_SECONDS", "300")
)
REAL_DOWNLOAD_MODEL = os.environ.get("LEMONADE_DOWNLOAD_TEST_MODEL", ENDPOINT_TEST_MODEL)
RUN_REAL_DOWNLOAD_TESTS = os.environ.get("LEMONADE_RUN_REAL_DOWNLOAD_TESTS", "").lower() in {
    "1",
    "true",
    "yes",
    "on",
}
RUN_GGUF_MAGIC_TESTS = os.environ.get("LEMONADE_RUN_GGUF_MAGIC_TESTS", "").lower() in {
    "1",
    "true",
    "yes",
    "on",
}

TERMINAL_STATUSES = {"completed", "cancelled", "error"}


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _git_blob_sha1_file(path: Path) -> str:
    digest = hashlib.sha1()
    size = path.stat().st_size
    digest.update(f"blob {size}\0".encode("ascii"))
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _repo_cache_dir_name(repo_id: str) -> str:
    return "models--" + repo_id.replace("/", "--")


def _checkpoint_parts(checkpoint: str) -> tuple[str, str]:
    if ":" in checkpoint:
        repo_id, variant = checkpoint.split(":", 1)
        return repo_id, variant
    return checkpoint, ""


def _assert_gguf_magic(testcase: ServerTestBase, path: Path) -> None:
    with path.open("rb") as handle:
        testcase.assertEqual(
            handle.read(4),
            b"GGUF",
            f"{path} must be a real GGUF file, not an HTML/pointer/error payload",
        )


def _flip_one_byte(path: Path, *, preserve_gguf_magic: bool) -> tuple[int, bytes]:
    size = path.stat().st_size
    if size <= 0:
        raise AssertionError(f"Cannot corrupt empty file: {path}")

    if preserve_gguf_magic:
        if size <= 8:
            raise AssertionError(f"File is too small to corrupt after GGUF magic: {path}")
        offset = min(max(8, size // 2), size - 1)
    else:
        offset = 0

    with path.open("r+b") as handle:
        handle.seek(offset)
        original = handle.read(1)
        if not original:
            raise AssertionError(f"Could not read byte at offset {offset} in {path}")
        handle.seek(offset)
        handle.write(bytes([original[0] ^ 0x01]))

    return offset, original


def _restore_one_byte(path: Path, offset: int, value: bytes) -> None:
    if not path.exists():
        return
    with path.open("r+b") as handle:
        handle.seek(offset)
        handle.write(value)


class ServerDownloadRegistryTests(ServerTestBase):
    """Tests the non-SSE model download registry path used by the desktop UI."""

    def _get_downloads(self):
        response = requests.get(
            f"http://localhost:{PORT}/api/v1/downloads",
            timeout=TIMEOUT_DEFAULT,
        )
        response.raise_for_status()
        data = response.json()
        self.assertIsInstance(data, list)
        return data

    def _get_job(self, download_id):
        return next((item for item in self._get_downloads() if item.get("id") == download_id), None)

    def _control_download(self, download_id, action):
        response = requests.post(
            f"http://localhost:{PORT}/api/v1/downloads/control",
            json={"id": download_id, "action": action},
            timeout=TIMEOUT_DEFAULT,
        )
        response.raise_for_status()
        return response.json()

    def _post_pull(self, model_name, *, stream=True, subscribe=False, do_not_upgrade=True, **extra):
        body = {
            "model": model_name,
            "stream": stream,
            "subscribe": subscribe,
            "do_not_upgrade": do_not_upgrade,
        }
        body.update(extra)
        response = requests.post(
            f"http://localhost:{PORT}/api/v1/pull",
            json=body,
            timeout=TIMEOUT_DEFAULT,
        )
        response.raise_for_status()
        return response.json()

    def _wait_for_job(self, download_id, predicate, description, timeout=DEFAULT_WAIT_SECONDS):
        deadline = time.time() + timeout
        last_job = None
        while time.time() < deadline:
            job = self._get_job(download_id)
            if job is not None:
                last_job = job
            if job is not None and predicate(job):
                return job
            time.sleep(DEFAULT_POLL_SECONDS)
        self.fail(
            f"Timed out waiting for {description}. Last snapshot: {last_job!r}"
        )

    def _wait_for_status(self, download_id, statuses, timeout=DEFAULT_WAIT_SECONDS):
        return self._wait_for_job(
            download_id,
            lambda job: job.get("status") in statuses and job.get("running") is not True,
            f"{download_id} to reach one of {sorted(statuses)}",
            timeout=timeout,
        )

    def _remove_row_if_present(self, download_id, *, allow_running=False):
        job = self._get_job(download_id)
        if not job:
            return
        if job.get("running") is True and not allow_running:
            self.fail(
                f"Refusing to cancel existing running download row for {download_id}. "
                "Choose another model or clean the row manually."
            )

        removed = self._control_download(download_id, "remove")
        if removed.get("running") is True:
            self._wait_for_status(download_id, TERMINAL_STATUSES, timeout=60)
            self._control_download(download_id, "remove")

        self.assertIsNone(self._get_job(download_id), "download row should be removable")

    def _wait_for_completed_download(self, model_name, *, timeout=REAL_DOWNLOAD_WAIT_SECONDS):
        download_id = f"model:{model_name}"
        self._remove_row_if_present(download_id)
        snapshot = self._post_pull(
            model_name,
            stream=True,
            subscribe=False,
            do_not_upgrade=False,
        )
        self.assertEqual(snapshot.get("id"), download_id)
        self.assertEqual(snapshot.get("type"), "model")
        self.assertEqual(snapshot.get("model_name"), model_name)

        job = self._wait_for_status(download_id, {"completed", "error"}, timeout=timeout)
        if job.get("status") == "error":
            self.fail(f"download failed: {job.get('error') or job!r}")
        self.assertEqual(job.get("status"), "completed")
        self.assertIs(job.get("running"), False)
        self._remove_row_if_present(download_id)
        return job

    def _get_model_info(self, model_name):
        response = requests.get(
            f"http://localhost:{PORT}/api/v1/models/{quote(model_name, safe='')}",
            timeout=TIMEOUT_DEFAULT,
        )
        response.raise_for_status()
        return response.json()

    def _find_downloaded_gguf(self, model_name) -> Path:
        info = self._get_model_info(model_name)
        checkpoint = info.get("checkpoint") or ""
        repo_id, variant = _checkpoint_parts(checkpoint)

        candidates: list[Path] = []
        cache_roots = [Path(path).expanduser() for path in get_hf_cache_dir_candidates()]

        if repo_id and "/" in repo_id:
            repo_cache = _repo_cache_dir_name(repo_id)
            for cache_root in cache_roots:
                snapshots = cache_root / repo_cache / "snapshots"
                if not snapshots.exists():
                    continue
                if variant:
                    candidates.extend(snapshots.glob(f"*/{variant}"))
                candidates.extend(snapshots.glob("*/*.gguf"))

        # Fallback for changed model metadata or hand-authored test models.
        if not candidates:
            for cache_root in cache_roots:
                if cache_root.exists():
                    candidates.extend(cache_root.glob("models--*/snapshots/*/*.gguf"))

        candidates = [path for path in candidates if path.is_file()]
        self.assertTrue(
            candidates,
            f"Could not find a downloaded GGUF file for {model_name}; checkpoint={checkpoint!r}, "
            f"cache_roots={[str(path) for path in cache_roots]}",
        )

        if variant:
            variant_matches = [path for path in candidates if path.name == variant]
            if variant_matches:
                candidates = variant_matches

        return max(candidates, key=lambda path: path.stat().st_mtime)

    def _assert_completed_job_schema(self, job):
        for field in ["id", "type", "model_name", "status", "running"]:
            self.assertIn(field, job)
        self.assertIsInstance(job["id"], str)
        self.assertEqual(job["type"], "model")
        self.assertIn(job["status"], {"downloading", "completed", "paused", "cancelled", "error"})
        self.assertIsInstance(job["running"], bool)

    def test_001_pull_subscribe_false_registers_download_and_control_can_remove(self):
        """POST /pull with subscribe=false registers a server-owned job visible via /downloads."""
        model_name = f"Definitely-Not-A-Real-Download-Test-Model-{uuid.uuid4().hex}"
        download_id = f"model:{model_name}"

        # Use an intentionally unknown model so the test exercises the registry
        # path without downloading a real model from Hugging Face. The initial
        # response should still be a JSON job snapshot, not an SSE stream.
        response = requests.post(
            f"http://localhost:{PORT}/api/v1/pull",
            json={
                "model_name": model_name,
                "stream": True,
                "subscribe": False,
            },
            timeout=TIMEOUT_DEFAULT,
        )
        response.raise_for_status()

        snapshot = response.json()
        self.assertEqual(snapshot.get("id"), download_id)
        self.assertEqual(snapshot.get("type"), "model")
        self.assertEqual(snapshot.get("model_name"), model_name)
        self.assertIn(snapshot.get("status"), {"downloading", "error"})

        terminal = self._wait_for_status(download_id, {"error"})
        self.assertIn("error", terminal)

        removed = self._control_download(download_id, "remove")
        self.assertEqual(removed.get("status"), "ok")

        downloads_after_remove = self._get_downloads()
        self.assertFalse(
            any(item.get("id") == download_id for item in downloads_after_remove),
            "removed download job should not remain visible",
        )

    def test_002_download_control_validates_required_fields(self):
        """The control endpoint rejects malformed requests with a client error."""
        response = requests.post(
            f"http://localhost:{PORT}/api/v1/downloads/control",
            json={"action": "remove"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 400)
        self.assertIn("error", response.json())

    def test_003_real_tiny_model_download_completes_and_exposes_terminal_snapshot(self):
        """Optional CI smoke test: a real small model reaches completed/running=false."""
        if not RUN_REAL_DOWNLOAD_TESTS:
            self.skipTest("set LEMONADE_RUN_REAL_DOWNLOAD_TESTS=1 to exercise real downloads")

        job = self._wait_for_completed_download(REAL_DOWNLOAD_MODEL)
        self._assert_completed_job_schema(job)
        self.assertEqual(job.get("percent"), 100)

        info = self._get_model_info(REAL_DOWNLOAD_MODEL)
        self.assertTrue(info.get("downloaded"), f"{REAL_DOWNLOAD_MODEL} should be marked downloaded")
        self.assertEqual(info.get("recipe"), "llamacpp")

        gguf = self._find_downloaded_gguf(REAL_DOWNLOAD_MODEL)
        _assert_gguf_magic(self, gguf)

    def test_004_real_download_redownloads_corrupted_gguf_magic(self):
        """Optional CI smoke test: cached HTML/pointer-like GGUF payloads are not trusted."""
        if not RUN_REAL_DOWNLOAD_TESTS:
            self.skipTest("set LEMONADE_RUN_REAL_DOWNLOAD_TESTS=1 to exercise real downloads")
        if not RUN_GGUF_MAGIC_TESTS:
            self.skipTest("set LEMONADE_RUN_GGUF_MAGIC_TESTS=1 to exercise the extra GGUF-magic cache check")

        self._wait_for_completed_download(REAL_DOWNLOAD_MODEL)
        gguf = self._find_downloaded_gguf(REAL_DOWNLOAD_MODEL)
        original_sha256 = _sha256_file(gguf)
        offset, original_byte = _flip_one_byte(gguf, preserve_gguf_magic=False)

        try:
            with gguf.open("rb") as handle:
                self.assertNotEqual(handle.read(4), b"GGUF")
            self._wait_for_completed_download(REAL_DOWNLOAD_MODEL)
            _assert_gguf_magic(self, gguf)
            self.assertEqual(
                _sha256_file(gguf),
                original_sha256,
                "pull should replace a cached file with broken GGUF magic, not trust it",
            )
        finally:
            if gguf.exists() and _sha256_file(gguf) != original_sha256:
                _restore_one_byte(gguf, offset, original_byte)

    def test_005_real_download_sha_verification_redownloads_corrupted_payload(self):
        """Optional CI smoke test: SHA/Git-SHA catches corruption beyond the GGUF magic header."""
        if not RUN_REAL_DOWNLOAD_TESTS:
            self.skipTest("set LEMONADE_RUN_REAL_DOWNLOAD_TESTS=1 to exercise real downloads")

        self._wait_for_completed_download(REAL_DOWNLOAD_MODEL)
        gguf = self._find_downloaded_gguf(REAL_DOWNLOAD_MODEL)
        _assert_gguf_magic(self, gguf)
        original_sha256 = _sha256_file(gguf)
        original_git_sha1 = _git_blob_sha1_file(gguf)

        offset, original_byte = _flip_one_byte(gguf, preserve_gguf_magic=True)
        corrupt_sha256 = _sha256_file(gguf)
        corrupt_git_sha1 = _git_blob_sha1_file(gguf)

        try:
            _assert_gguf_magic(self, gguf)
            self.assertNotEqual(corrupt_sha256, original_sha256)
            self.assertNotEqual(corrupt_git_sha1, original_git_sha1)

            download_id = f"model:{REAL_DOWNLOAD_MODEL}"
            self._remove_row_if_present(download_id)
            snapshot = self._post_pull(
                REAL_DOWNLOAD_MODEL,
                stream=True,
                subscribe=False,
                do_not_upgrade=False,
            )
            self.assertEqual(snapshot.get("id"), download_id)

            job = self._wait_for_status(
                download_id,
                {"completed", "error"},
                timeout=REAL_DOWNLOAD_WAIT_SECONDS,
            )
            if job.get("status") == "error":
                error = str(job.get("error") or job)
                self.assertIn(
                    "verification",
                    error.lower(),
                    "hash failures must surface an explicit verification error",
                )
                self.fail(f"download hash verification failed as expected but did not recover: {error}")

            self.assertEqual(job.get("status"), "completed")
            self._remove_row_if_present(download_id)

            _assert_gguf_magic(self, gguf)
            self.assertEqual(
                _sha256_file(gguf),
                original_sha256,
                "pull should redownload the expected bytes after SHA/Git-SHA corruption",
            )
            self.assertEqual(_git_blob_sha1_file(gguf), original_git_sha1)
        finally:
            if gguf.exists() and _sha256_file(gguf) != original_sha256:
                _restore_one_byte(gguf, offset, original_byte)

    def test_006_real_download_reuses_valid_verified_cache_without_redownload(self):
        """Optional CI/local smoke test: a valid cached GGUF is verified and reused, not redownloaded."""
        if not RUN_REAL_DOWNLOAD_TESTS:
            self.skipTest("set LEMONADE_RUN_REAL_DOWNLOAD_TESTS=1 to exercise real downloads")

        self._wait_for_completed_download(REAL_DOWNLOAD_MODEL)
        gguf = self._find_downloaded_gguf(REAL_DOWNLOAD_MODEL)
        _assert_gguf_magic(self, gguf)

        original_sha256 = _sha256_file(gguf)
        original_git_sha1 = _git_blob_sha1_file(gguf)
        original_size = gguf.stat().st_size
        original_mtime_ns = gguf.stat().st_mtime_ns

        # Pull the same model again. With a valid final cache file and matching
        # SHA/Git-SHA metadata, HttpClient should verify and return success
        # without replacing the final file.
        job = self._wait_for_completed_download(REAL_DOWNLOAD_MODEL)
        self.assertEqual(job.get("status"), "completed")

        reused_gguf = self._find_downloaded_gguf(REAL_DOWNLOAD_MODEL)
        self.assertEqual(reused_gguf, gguf)
        self.assertEqual(reused_gguf.stat().st_size, original_size)
        self.assertEqual(reused_gguf.stat().st_mtime_ns, original_mtime_ns)
        self.assertEqual(_sha256_file(reused_gguf), original_sha256)
        self.assertEqual(_git_blob_sha1_file(reused_gguf), original_git_sha1)


if __name__ == "__main__":
    run_server_tests(ServerDownloadRegistryTests, "SERVER DOWNLOAD REGISTRY TESTS")
