import datetime
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import version

UTC = datetime.timezone.utc


class ReleaseWeekTests(unittest.TestCase):
    def test_before_wednesday_cutoff_targets_next_week(self):
        now = datetime.datetime(2026, 9, 9, 18, 59, tzinfo=UTC)
        self.assertEqual(version.upcoming_release_week(now), (2026, 38))

    def test_at_wednesday_cutoff_targets_week_after_next(self):
        now = datetime.datetime(2026, 9, 9, 19, 0, tzinfo=UTC)
        self.assertEqual(version.upcoming_release_week(now), (2026, 39))

    def test_iso_year_follows_release_date(self):
        now = datetime.datetime(2026, 12, 30, 20, 0, tzinfo=UTC)
        self.assertEqual(version.upcoming_release_week(now), (2027, 2))


class VersionTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp_dir.name)
        self.git("init", "-b", "main")
        self.git("config", "user.email", "version-test@example.com")
        self.git("config", "user.name", "Version Test")
        self.commit("initial")

    def tearDown(self):
        self.temp_dir.cleanup()

    def git(self, *args):
        return subprocess.run(
            ["git", *args],
            cwd=self.repo,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        ).stdout.strip()

    def commit(self, message):
        marker = self.repo / "marker"
        marker.write_text(message, encoding="utf-8")
        self.git("add", "marker")
        self.git("commit", "-m", message)

    def test_version_file_overrides_git(self):
        (self.repo / ".version").write_text("vcustom-version\n", encoding="utf-8")
        self.assertEqual(version.get_version(self.repo), "vcustom-version")

    def test_non_candidate_version_contains_count_and_hash(self):
        now = datetime.datetime(2026, 9, 10, 12, 0, tzinfo=UTC)
        expected_hash = self.git("rev-parse", "--short=8", "HEAD")
        self.assertEqual(
            version.get_version(self.repo, now), f"2026.39.0~1.{expected_hash}"
        )

    def test_release_branch_counts_commits_after_main(self):
        self.git("checkout", "-b", "release-v2026.38")
        self.commit("candidate one")
        self.commit("candidate two")
        self.assertEqual(version.get_version(self.repo), "2026.38.2")

    def test_release_branch_continues_from_existing_tag(self):
        self.git("checkout", "-b", "release-v2026.38")
        self.commit("candidate one")
        self.git("tag", "v2026.38.1")
        self.git("checkout", "main")
        self.git("merge", "--no-ff", "-s", "ours", "release-v2026.38")
        self.git("checkout", "release-v2026.38")
        self.commit("hotfix")
        self.assertEqual(version.get_version(self.repo), "2026.38.2")

    def test_exact_release_tag_works_in_detached_checkout(self):
        self.git("tag", "v2026.38.4")
        self.git("checkout", "--detach")
        with mock.patch.dict("os.environ", {}, clear=True):
            self.assertEqual(version.get_version(self.repo), "2026.38.4")

    def test_github_branch_name_works_in_detached_checkout(self):
        self.git("checkout", "-b", "release-v2026.38")
        self.commit("candidate one")
        self.git("checkout", "--detach")
        environment = {
            "GITHUB_REF_NAME": "release-v2026.38",
            "GITHUB_REF_TYPE": "branch",
        }
        with mock.patch.dict("os.environ", environment, clear=True):
            self.assertEqual(version.get_version(self.repo), "2026.38.1")


if __name__ == "__main__":
    unittest.main()
