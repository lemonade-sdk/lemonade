import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import source_archive, version


class SourceArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp_dir.name) / "repo"
        self.output_dir = Path(self.temp_dir.name) / "output"
        self.repo.mkdir()
        self.git("init", "-b", "main")
        self.git("config", "user.email", "archive-test@example.com")
        self.git("config", "user.name", "Archive Test")
        (self.repo / "tracked.txt").write_text("tracked\n", encoding="utf-8")
        self.git("add", "tracked.txt")
        self.git("commit", "-m", "initial")

    def tearDown(self):
        self.temp_dir.cleanup()

    def git(self, *args):
        subprocess.run(
            ["git", *args],
            cwd=self.repo,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def test_archive_contains_exact_version_stamp(self):
        result = source_archive.create_source_archive(
            self.repo, self.output_dir, "2026.38.2"
        )

        self.assertEqual(result.name, "lemonade-2026.38.2.tar.gz")
        extract_dir = Path(self.temp_dir.name) / "extracted"
        extracted_repo = extract_dir / "lemonade-2026.38.2"
        with tarfile.open(result, "r:gz") as archive:
            names = archive.getnames()
            version_path = "lemonade-2026.38.2/.version"
            self.assertEqual(names.count(version_path), 1)
            version_data = archive.extractfile(version_path).read()
            self.assertEqual(version_data, b"2026.38.2\n")
            self.assertIn("lemonade-2026.38.2/tracked.txt", names)
            self.assertFalse(any("/.git/" in name for name in names))

        extracted_repo.mkdir(parents=True)
        (extracted_repo / ".version").write_bytes(version_data)
        self.assertFalse((extracted_repo / ".git").exists())
        self.assertEqual(version.get_version(extracted_repo), "2026.38.2")

    def test_existing_version_file_is_replaced(self):
        (self.repo / ".version").write_text("old-version\n", encoding="utf-8")
        self.git("add", ".version")
        self.git("commit", "-m", "add old version")

        result = source_archive.create_source_archive(
            self.repo, self.output_dir, "2026.38.3"
        )

        with tarfile.open(result, "r:gz") as archive:
            version_path = "lemonade-2026.38.3/.version"
            self.assertEqual(archive.getnames().count(version_path), 1)
            self.assertEqual(archive.extractfile(version_path).read(), b"2026.38.3\n")

    @mock.patch("tools.source_archive.get_version", return_value="2026.38.4")
    def test_version_helper_is_used_by_default(self, get_version):
        result = source_archive.create_source_archive(self.repo, self.output_dir)

        get_version.assert_called_once_with(self.repo.resolve())
        self.assertEqual(result.name, "lemonade-2026.38.4.tar.gz")


if __name__ == "__main__":
    unittest.main()
