"""Offline tests for modelscope_mirror.py. Run: python tools/modelscope_mirror/test_modelscope_mirror.py"""

import hashlib
import http.server
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

import modelscope_mirror as m

VERSIONS = {
    "comment": "x",
    "checksums": {
        "github": {
            "lemonade-sdk/ort-server": {"0.4.0": {"a.zip": "sha256:" + "a" * 64}}
        },
        "therock": {"10.1.0": {"t.tar.gz": "sha256:" + "b" * 64}},
    },
    "llamacpp": {
        "comment": "c",
        "cpu": "b1",
        "nathanw": "img@sha256:" + "0" * 64,
    },
    "onnxruntime": {"cpu": "0.4.0"},
    "vllm": {"rocm": "v1", "rocm_arch_overrides": {"gfx942": "x"}},
    "trellis": {"cuda": "v0.8.1"},
    "halogen": {"rocm": "1@sha256:" + "0" * 64},
    "therock": {"version": "10.1.0"},
}


class PinsTest(unittest.TestCase):
    def test_pins_skip_containers_and_comments(self):
        pins = m.pinned_releases(VERSIONS, None, include_pwilkin=False)
        self.assertEqual(
            set(pins),
            {("ggml-org/llama.cpp", "b1"), ("lemonade-sdk/ort-server", "0.4.0")}
            | {("lemonade-sdk/vllm-rocm", f"v1-{f}") for f in m.VLLM_FAMILIES},
        )
        with_pwilkin = m.pinned_releases(VERSIONS, None, include_pwilkin=True)
        self.assertIn(("pwilkin/trellis.cpp", "v0.8.1"), with_pwilkin)

    def test_unknown_backend_stops_the_mirror(self):
        with self.assertRaises(SystemExit):
            m.pinned_releases({"newbackend": {"cpu": "v1"}}, None, True)

    def test_pinned_sha256_reads_both_sections(self):
        self.assertEqual(
            m.pinned_sha256(
                [VERSIONS], "github/lemonade-sdk/ort-server/0.4.0", "a.zip"
            ),
            "a" * 64,
        )
        self.assertEqual(
            m.pinned_sha256([VERSIONS], "therock/10.1.0", "t.tar.gz"), "b" * 64
        )
        self.assertEqual(m.pinned_sha256([VERSIONS], "therock/10.1.0", "other"), "")

    def test_pins_that_disagree_stop_the_mirror(self):
        other = {
            "checksums": {"therock": {"10.1.0": {"t.tar.gz": "sha256:" + "c" * 64}}}
        }
        with self.assertRaises(SystemExit):
            m.pinned_sha256([VERSIONS, other], "therock/10.1.0", "t.tar.gz")

    def test_upstream_change_after_pinning_stops_the_mirror(self):
        item = m.Item("small", "github/lemonade-sdk/ort-server/0.4.0", "src")
        item.files.append({"name": "a.zip", "size": 1, "sha256": "f" * 64, "url": "u"})
        versions = {"checksums": VERSIONS["checksums"], "onnxruntime": {"cpu": "0.4.0"}}
        with mock.patch.object(m, "release_item", return_value=item):
            with self.assertRaises(SystemExit):
                m.build_plan([versions], "trial", False, "none")

    def test_describe_drift(self):
        want = {"github": {"r": {"t": {"a": "1", "b": "2"}}}}
        have = {"github": {"r": {"t": {"a": "9", "c": "3"}}}}
        self.assertEqual(
            m.describe_drift(want, have),
            [
                "  differs  checksums.github.r.t.a: pinned 9, upstream 1",
                "  missing  checksums.github.r.t.b",
                "  stale    checksums.github.r.t.c",
            ],
        )


class DownloadTest(unittest.TestCase):
    DATA = bytes(range(256)) * 4000

    def serve(self, drops: int):
        calls = []
        data = self.DATA

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                calls.append(self.headers.get("Range"))
                start = (
                    int(self.headers["Range"][6:-1]) if self.headers.get("Range") else 0
                )
                self.send_response(206 if start else 200)
                body = data[start:]
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body if len(calls) > drops else body[: len(body) // 3])
                self.close_connection = True

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.shutdown)
        return f"http://127.0.0.1:{server.server_port}/f", calls

    def setUp(self):
        patcher = mock.patch.object(m.time, "sleep")
        patcher.start()
        self.addCleanup(patcher.stop)
        self.dest = Path(tempfile.mkdtemp()) / "f.bin"

    def test_resumes_after_dropped_connections(self):
        url, calls = self.serve(drops=2)
        sha = hashlib.sha256(self.DATA).hexdigest()
        self.assertEqual(m.download(url, self.dest, sha, len(self.DATA)), sha)
        self.assertEqual(calls[0], None)
        self.assertTrue(all(r and r.startswith("bytes=") for r in calls[1:]))

    def test_rejects_wrong_sha256(self):
        url, _ = self.serve(drops=0)
        with self.assertRaises(RuntimeError):
            m.download(url, self.dest, "0" * 64, len(self.DATA))


if __name__ == "__main__":
    unittest.main()
