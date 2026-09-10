#!/usr/bin/env python3

import argparse
import gzip
import io
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

try:
    from tools.version import get_version, git
except ModuleNotFoundError:
    from version import get_version, git


def create_source_archive(repo, output_dir, version=None):
    repo = Path(repo).resolve()
    output_dir = Path(output_dir).resolve()
    version = version or get_version(repo)
    if not version or "/" in version or "\\" in version:
        raise ValueError(f"invalid archive version: {version!r}")

    archive_root = f"lemonade-{version}"
    output_dir.mkdir(parents=True, exist_ok=True)
    destination = output_dir / f"{archive_root}.tar.gz"

    with tempfile.TemporaryDirectory() as temp_dir:
        raw_archive = Path(temp_dir) / "source.tar"
        subprocess.run(
            [
                "git",
                "archive",
                "--format=tar",
                f"--prefix={archive_root}/",
                f"--output={raw_archive}",
                "HEAD",
                "--",
                ".",
                ":(exclude).version",
            ],
            cwd=repo,
            check=True,
        )

        version_data = f"{version}\n".encode()
        version_info = tarfile.TarInfo(f"{archive_root}/.version")
        version_info.size = len(version_data)
        version_info.mode = 0o644
        version_info.mtime = int(git("show", "-s", "--format=%ct", "HEAD", cwd=repo))
        with tarfile.open(raw_archive, "a") as archive:
            archive.addfile(version_info, fileobj=io.BytesIO(version_data))

        with raw_archive.open("rb") as source, destination.open("wb") as output:
            with gzip.GzipFile(
                filename="", mode="wb", fileobj=output, mtime=0
            ) as compressed:
                shutil.copyfileobj(source, compressed)

    return destination


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, default=Path.cwd())
    parser.add_argument("--version")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    try:
        print(create_source_archive(repo, args.output_dir, args.version))
    except (
        OSError,
        ValueError,
        subprocess.CalledProcessError,
        tarfile.TarError,
    ) as error:
        print(f"error: could not create source archive: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
