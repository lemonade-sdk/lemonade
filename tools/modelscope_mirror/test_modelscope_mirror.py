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
        "modelscope": {
            "github": {
                "lemonade-sdk/ort-server": {"0.4.0": {"a.zip": "sha256:" + "a" * 64}}
            },
            "therock": {"10.1.0": {"t.tar.gz": "sha256:" + "b" * 64}},
        },
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

    def test_unknown_backend_is_skipped_not_fatal(self):
        versions = {"newbackend": {"cpu": "v1"}, "onnxruntime": {"cpu": "0.4.0"}}
        self.assertEqual(
            m.pinned_releases(versions, None, True),
            {("lemonade-sdk/ort-server", "0.4.0"): "small"},
        )

    def test_therock_targets_merge_across_pin_files(self):
        a = {"therock": {"version": "10.2.0", "architectures": ["gfx1150"]}}
        b = {"therock": {"version": "10.2.0", "architectures": ["gfx1151"]}}
        seen = []
        with mock.patch.object(
            m,
            "therock_item",
            side_effect=lambda v: seen.append(v) or m.Item("therock", "t", "s"),
        ):
            m.build_plan([a, b], "core", False, "none", only_group="therock")
        self.assertEqual(len(seen), 1)
        self.assertEqual(seen[0]["therock"]["architectures"], ["gfx1150", "gfx1151"])

    def test_hand_pins_count_for_github_files(self):
        hand = {
            "checksums": {"github": {"o/r": {"v1": {"f.zip": "sha256:" + "d" * 64}}}}
        }
        self.assertEqual(m.pinned_sha256([hand], "github/o/r/v1", "f.zip"), "d" * 64)

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
            "checksums": {
                "modelscope": {
                    "therock": {"10.1.0": {"t.tar.gz": "sha256:" + "c" * 64}}
                }
            }
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

    def test_checksummed_only_waits_for_checksums(self):
        hashed = m.Item("small", "github/lemonade-sdk/ort-server/0.4.0", "src")
        hashed.files.append({"name": "a.zip", "size": 1, "sha256": "", "url": "u"})
        unhashed = m.Item("small", "github/o/r/v9", "src")
        unhashed.files.append({"name": "b.zip", "size": 1, "sha256": "", "url": "u"})
        installers = m.Item("small", f"github/{m.LEMONADE_REPO}/v1", "src")
        installers.files.append(
            {"name": "lemonade.msi", "size": 1, "sha256": "", "url": "u"}
        )
        new_rock = m.Item("therock", "therock/10.2.0", "src")
        new_rock.files.append({"name": "t.tar.gz", "size": 1, "sha256": "", "url": "u"})
        rock = m.Item("therock", "therock/10.1.0", "src")
        rock.files += [
            {"name": "t.tar.gz", "size": 1, "sha256": "", "url": "u"},
            {"name": "unhashed.tar.gz", "size": 1, "sha256": "", "url": "u"},
        ]
        kept = m.checksummed_only(
            [hashed, unhashed, installers, new_rock, rock], [VERSIONS]
        )
        self.assertEqual(
            [i.folder for i in kept], [hashed.folder, installers.folder, rock.folder]
        )
        self.assertEqual([f["name"] for f in rock.files], ["t.tar.gz"])

    def test_new_container_backends_are_skipped(self):
        versions = {"aim": {"rocm": "2026.11.0@sha256:" + "0" * 64}}
        self.assertEqual(m.pinned_releases(versions, None, True), {})

    def test_describe_drift(self):
        want = {"github": {"r": {"t": {"a": "1", "b": "2"}, "new": {"x": "4"}}}}
        have = {"github": {"r": {"t": {"a": "9", "c": "3"}, "old": {"y": "5"}}}}
        self.assertEqual(
            m.describe_drift(want, have),
            [
                (True, "  missing  checksums.github.r.new"),
                (False, "  stale    checksums.github.r.old"),
                (True, "  differs  checksums.github.r.t.a: pinned 9, upstream 1"),
                (False, "  missing  checksums.github.r.t.b"),
                (False, "  stale    checksums.github.r.t.c"),
            ],
        )

    def test_write_adds_late_assets_and_keeps_pinned_hashes(self):
        item = m.Item("small", "github/lemonade-sdk/ort-server/0.4.0", "src")
        item.files += [
            {"name": "a.zip", "size": 1, "sha256": "f" * 64, "url": "u"},
            {"name": "late.zip", "size": 1, "sha256": "e" * 64, "url": "u"},
        ]
        versions = {"checksums": VERSIONS["checksums"], "onnxruntime": {"cpu": "0.4.0"}}
        versions["therock"] = VERSIONS["therock"]
        with mock.patch.object(m, "release_item", return_value=item), mock.patch.object(
            m, "therock_item", side_effect=AssertionError("AMD contacted")
        ):
            out = m.checksums_for(versions, False, True, Path("."))
        self.assertEqual(
            out["github"]["lemonade-sdk/ort-server"]["0.4.0"],
            {"a.zip": "sha256:" + "a" * 64, "late.zip": "sha256:" + "e" * 64},
        )
        self.assertEqual(out["therock"], VERSIONS["checksums"]["modelscope"]["therock"])

    def test_new_therock_version_is_hashed_from_amd(self):
        versions = {
            "checksums": VERSIONS["checksums"],
            "therock": {"version": "10.2.0"},
        }
        rock = m.Item("therock", "therock/10.2.0", "src")
        rock.files += [
            {"name": "a.tar.gz", "size": 1, "sha256": "", "url": "ua"},
            {"name": "b.tar.gz", "size": 1, "sha256": "", "url": "ub"},
        ]
        with mock.patch.object(m, "therock_item", return_value=rock), mock.patch.object(
            m, "download", side_effect=lambda url, *_: url[-1] * 64
        ):
            out = m.checksums_for(versions, False, True, Path("."))
            check = m.checksums_for(versions, True, False, Path("."))
        self.assertEqual(
            out["therock"],
            {
                "10.2.0": {
                    "a.tar.gz": "sha256:" + "a" * 64,
                    "b.tar.gz": "sha256:" + "b" * 64,
                }
            },
        )
        self.assertEqual(
            check["therock"],
            {
                "10.2.0": {
                    "a.tar.gz": "(not hashed yet)",
                    "b.tar.gz": "(not hashed yet)",
                }
            },
        )
        fatal = [
            line
            for is_fatal, line in m.describe_drift(
                {"therock": check["therock"]},
                {"therock": VERSIONS["checksums"]["modelscope"]["therock"]},
            )
            if is_fatal
        ]
        self.assertEqual(fatal, ["  missing  checksums.therock.10.2.0"])

    def test_no_published_therock_tarballs_stops_the_write(self):
        versions = {
            "checksums": VERSIONS["checksums"],
            "therock": {"version": "10.2.0"},
        }
        with mock.patch.object(
            m, "therock_item", return_value=m.Item("therock", "therock/10.2.0", "src")
        ):
            with self.assertRaises(SystemExit):
                m.checksums_for(versions, False, True, Path("."))


class ManifestTest(unittest.TestCase):
    def item(self):
        item = m.Item("small", "github/o/r/v1", "src")
        item.files.append({"name": "a.zip", "size": 3, "sha256": "a" * 64, "url": "u"})
        return item

    def test_manifest_matches_only_the_planned_files(self):
        item = self.item()
        self.assertTrue(m.manifest_is_current(item, m.manifest_files(item)))
        self.assertFalse(m.manifest_is_current(item, None))
        self.assertFalse(m.manifest_is_current(item, []))
        stale = [{"name": "a.zip", "size": 3, "sha256": "b" * 64}]
        self.assertFalse(m.manifest_is_current(item, stale))

    def test_verify_reports_an_outdated_manifest(self):
        item = self.item()
        existing = {"github/o/r/v1/a.zip": 3, "github/o/r/v1/assets.json": 1}
        self.assertEqual(
            m.missing_from([item], existing, lambda folder: []),
            ["github/o/r/v1/assets.json (out of date)"],
        )
        self.assertEqual(
            m.missing_from([item], existing, lambda folder: m.manifest_files(item)), []
        )


class RedirectTest(unittest.TestCase):
    def test_credentials_are_not_forwarded_to_another_host(self):
        seen = {}

        class Target(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                seen["auth"] = self.headers.get("Authorization")
                seen["cookie"] = self.headers.get("Cookie")
                self.send_response(200)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"ok")

        target = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Target)
        port = target.server_port

        class Origin(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self.send_response(302)
                self.send_header("Location", f"http://localhost:{port}/file")
                self.end_headers()

        origin = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Origin)
        for server in (target, origin):
            threading.Thread(target=server.serve_forever, daemon=True).start()
            self.addCleanup(server.shutdown)
        req = m.urllib.request.Request(
            f"http://127.0.0.1:{origin.server_port}/",
            headers={"Authorization": "Bearer secret", "Cookie": "s=secret"},
        )
        with m.urllib.request.urlopen(req, timeout=10) as r:
            self.assertEqual(r.read(), b"ok")
        self.assertEqual(seen, {"auth": None, "cookie": None})


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
