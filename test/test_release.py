import subprocess
import tempfile
import unittest
from pathlib import Path

from tools import release


class ReleaseHelperTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        root = Path(self.temp_dir.name)
        self.remote = root / "remote.git"
        self.repo = root / "clone"

        self.run_cmd("git", "init", "--bare", "-b", "main", str(self.remote), cwd=root)
        self.run_cmd("git", "clone", str(self.remote), str(self.repo), cwd=root)
        self.git("config", "user.email", "release-test@example.com")
        self.git("config", "user.name", "Release Test")
        self.git("config", "commit.gpgsign", "false")
        self.commit("initial")
        self.git("push", "origin", "main")

    def tearDown(self):
        self.temp_dir.cleanup()

    def run_cmd(self, *args, cwd):
        return subprocess.run(
            args,
            cwd=cwd,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        ).stdout.strip()

    def git(self, *args):
        return self.run_cmd("git", *args, cwd=self.repo)

    def commit(self, message):
        marker = self.repo / "marker"
        marker.write_text(message, encoding="utf-8")
        self.git("add", "marker")
        self.git("commit", "-m", message)

    def push_release_branch(self, name, commits):
        self.git("checkout", "-b", name, "main")
        for index in range(commits):
            self.commit(f"{name} candidate {index}")
        self.git("push", "origin", name)
        self.git("checkout", "main")

    def test_select_branch_picks_newest_by_year_week(self):
        self.push_release_branch("release-v2026.34", 1)
        self.push_release_branch("release-v2026.38", 1)
        self.push_release_branch("release-v2025.50", 1)
        self.git("fetch", "--prune", "origin")
        self.assertEqual(
            release.select_branch(self.repo, "origin", None), "release-v2026.38"
        )

    def test_select_branch_honors_explicit_hotfix_branch(self):
        self.push_release_branch("release-v2026.34", 1)
        self.push_release_branch("release-v2026.38", 1)
        self.git("fetch", "--prune", "origin")
        self.assertEqual(
            release.select_branch(self.repo, "origin", "release-v2026.34"),
            "release-v2026.34",
        )

    def test_missing_explicit_branch_raises(self):
        self.push_release_branch("release-v2026.38", 1)
        self.git("fetch", "--prune", "origin")
        with self.assertRaises(RuntimeError):
            release.select_branch(self.repo, "origin", "release-v1999.1")

    def test_dry_run_computes_version_without_tagging(self):
        self.push_release_branch("release-v2026.38", 3)
        release.create_release(
            self.repo,
            "origin",
            None,
            "Lemonade {tag}",
            assume_yes=True,
            dry_run=True,
        )
        self.assertEqual(self.git("tag", "--list"), "")

    def test_create_release_tags_and_pushes(self):
        self.push_release_branch("release-v2026.38", 3)
        release.create_release(
            self.repo,
            "origin",
            None,
            "Lemonade {tag}",
            assume_yes=True,
            dry_run=False,
            sign=False,
        )
        self.assertIn("v2026.38.3", self.git("tag", "--list").splitlines())
        pushed = self.run_cmd(
            "git", "ls-remote", "--tags", str(self.remote), "v2026.38.3", cwd=self.repo
        )
        self.assertIn("v2026.38.3", pushed)

    def test_existing_tag_is_rejected(self):
        self.push_release_branch("release-v2026.38", 3)
        self.git("fetch", "origin")
        tip = self.git("rev-parse", "origin/release-v2026.38")
        self.git("tag", "v2026.38.3", tip)
        self.git("push", "origin", "v2026.38.3")
        with self.assertRaises(RuntimeError):
            release.create_release(
                self.repo,
                "origin",
                None,
                "Lemonade {tag}",
                assume_yes=True,
                dry_run=False,
            )


if __name__ == "__main__":
    unittest.main()
