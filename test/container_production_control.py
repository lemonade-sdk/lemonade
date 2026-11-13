"""Build the pinned C7b source for immutable transition replay."""

import argparse
import io
import os
from pathlib import Path
import subprocess
import tarfile

REPO = Path(__file__).resolve().parents[1]
BASE = "3af47274a644571d29b5af9b6b2b780c04864371"


def build(root, dependencies):
    source = root / "source"
    source.mkdir(parents=True, exist_ok=True)
    archive = subprocess.check_output(["git", "archive", BASE], cwd=REPO, timeout=60)
    with tarfile.open(fileobj=io.BytesIO(archive)) as tree:
        for member in tree:
            if member.isfile():
                path = source / member.name
                data = tree.extractfile(member).read()
                if not path.exists():
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
                assert path.read_bytes() == data, "C8PinnedReplaySource:" + member.name
    temporary = root / "temporary"
    temporary.mkdir(exist_ok=True)
    environment = dict(
        os.environ, TMPDIR=str(temporary), TMP=str(temporary), TEMP=str(temporary)
    )
    subprocess.run(
        [
            "cmake",
            "-S",
            str(source),
            "-B",
            str(root / "build"),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DBUILD_WEB_APP=OFF",
            "-DREQUIRE_LINUX_TRAY=OFF",
            "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
            f"-DFETCHCONTENT_BASE_DIR={root / 'dependencies'}",
            *[
                f"-DFETCHCONTENT_SOURCE_DIR_{name.upper()}={dependencies / (name + '-src')}"
                for name in ("mbedtls", "httplib", "cli11", "json")
            ],
        ],
        cwd=REPO,
        env=environment,
        check=True,
        timeout=120,
    )
    subprocess.run(
        [
            "cmake",
            "--build",
            str(root / "build"),
            "--target",
            "lemond",
            "lemonade",
            "test_llamacpp_v2_production_switch",
            "-j",
            "6",
        ],
        cwd=REPO,
        env=environment,
        check=True,
        timeout=900,
    )
    print("Pinned C7b replay control:", BASE)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path, required=True)
    args = parser.parse_args()
    build(args.root.resolve(), args.dependencies.resolve())
