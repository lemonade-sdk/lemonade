#!/usr/bin/env python3
"""Unit tests for .github/scripts/release_channels.py."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("release_channels.py")
SPEC = importlib.util.spec_from_file_location("release_channels", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
release_channels = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = release_channels
SPEC.loader.exec_module(release_channels)


class ReleaseChannelTests(unittest.TestCase):
    def test_supported_tags(self) -> None:
        base = "12.0.0"
        stable = release_channels.parse_tag("v12.0.0", base)
        beta = release_channels.parse_tag("v12.0.0-beta.2", base)
        self.assertTrue(stable.is_stable)
        self.assertEqual(beta.prerelease_suffix, "beta.2")

    def test_invalid_tags(self) -> None:
        invalid = [
            "v012.0.0",
            "v12.0.0-beta.01",
            "v12.0.0-beta.2oops",
            "v12.0.0-preview.1",
            "v12.0.0+build1",
        ]
        for tag in invalid:
            with self.subTest(tag=tag):
                with self.assertRaises(release_channels.ReleaseChannelError):
                    release_channels.parse_tag(tag, "12.0.0")

    def test_monotonic_serial_is_independent_of_tag_selection(self) -> None:
        base = "12.0.0"
        alpha = release_channels.parse_tag("v12.0.0-alpha.1", base)
        beta = release_channels.parse_tag("v12.0.0-beta.2", base)
        rc = release_channels.parse_tag("v12.0.0-rc.1", base)
        versions = [
            release_channels.derive_debian_version(
                base, None, "100", "1", "aaaaaaa"
            )[0],
            release_channels.derive_debian_version(
                base, alpha, "101", "1", "bbbbbbb"
            )[0],
            release_channels.derive_debian_version(
                base, beta, "102", "1", "ccccccc"
            )[0],
            release_channels.derive_debian_version(
                base, None, "103", "1", "ddddddd"
            )[0],
            release_channels.derive_debian_version(
                base, rc, "104", "1", "eeeeeee"
            )[0],
            release_channels.derive_debian_version(
                base, None, "104", "2", "fffffff"
            )[0],
            base,
        ]
        for left, right in zip(versions, versions[1:]):
            completed = subprocess.run(
                ["dpkg", "--compare-versions", left, "lt", right],
                check=False,
            )
            self.assertEqual(completed.returncode, 0, f"{left} !< {right}")

    def test_same_commit_tag_type_and_invalid_extra_tag_do_not_matter(self) -> None:
        # Version derivation consumes the explicitly validated event tag and the
        # workflow serial. It never searches repository tags, so annotated vs.
        # lightweight tags and unrelated invalid tags cannot change the result.
        base = "12.0.0"
        rc = release_channels.parse_tag("v12.0.0-rc.1", base)
        version, _ = release_channels.derive_debian_version(
            base, rc, "42", "1", "abcdef0"
        )
        self.assertEqual(version, "12.0.0~0.42.1.rc.1")

    def test_stable_always_supersedes_bleeding_edge(self) -> None:
        prerelease = release_channels.parse_tag("v12.0.0-rc.99", "12.0.0")
        bleeding, _ = release_channels.derive_debian_version(
            "12.0.0", prerelease, "999999", "9", "abcdef0"
        )
        self.assertEqual(
            subprocess.run(
                ["dpkg", "--compare-versions", bleeding, "lt", "12.0.0"],
                check=False,
            ).returncode,
            0,
        )

    def test_require_flags_are_mutually_exclusive(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir)
            (repo / "CMakeLists.txt").write_text(
                "project(lemon_cpp VERSION 12.0.0)\n", encoding="utf-8"
            )
            subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
            subprocess.run(
                ["git", "config", "user.email", "test@example.invalid"],
                cwd=repo,
                check=True,
            )
            subprocess.run(
                ["git", "config", "user.name", "Test"], cwd=repo, check=True
            )
            subprocess.run(["git", "add", "CMakeLists.txt"], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "init"], cwd=repo, check=True)
            subprocess.run(["git", "tag", "v12.0.0"], cwd=repo, check=True)
            with self.assertRaises(release_channels.ReleaseChannelError):
                release_channels.classify(
                    repository=repo,
                    tag_name="v12.0.0",
                    require_tag=True,
                    require_prerelease=True,
                    require_stable=True,
                    build_serial="1",
                    build_attempt="1",
                    commit_sha="abcdef0",
                )


if __name__ == "__main__":
    unittest.main(verbosity=2)
