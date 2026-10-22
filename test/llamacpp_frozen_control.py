"""Reconstruct and build the immutable, independent llama.cpp oracle control."""

import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

REPO = Path(__file__).resolve().parents[1]
BASE = "ab3946f6cf45632b019b5380872b14baa3027e7e"
ROOT = REPO / ".synagent/scratch/backend-v2-4e3a/frozen-control"
CONTROL_VERSION = "2026.45.0~1777.ab3946f6"
GENERATED_SHA256 = {
    "lemon/version.h": "f8e2957d359cf72c88c8f2d02c2501a05319bd7187efbe45553c67dbd4cfac1d",
    "backend_builtin_source_resources.h": "6ae4b948ebbd5b38cad3ae899be8a67a96fedb6d366f24b013a82071ec7e4c1b",
    "backend_capabilities_generated.h": "67933e46e3f6028b41cb5112ecdfc20a1388238c912b089238d210a9b0b35fd2",
    "backend_definition_resources.h": "61b7f9161e726cc6f90f37d44c6ff9ccf276ef62562cad001fc1cc787a1e5a2a",
    "backend_definition_v2_descriptor.h": "cd680a0fedc63954f37278df58b245c60f33cbc8a62c0dcb69af95bfcc43d763",
    "backend_definition_v2_resources.h": "8931979541cbd9dce01b2c01397f9d7acd123a92a9488c6865b3529f8aaa88e0",
    "backend_descriptors_generated.h": "593a54daf91789ebb782d2ce9601a014e7e77555d20e6f160d29ad7e2c64393b",
    "backend_factories_generated.h": "877b1aedecdc272fe1a8230fa9bf4eac7dadd07805d06ff4d94cdcfe8c4a46d4",
}

TARGETS = [
    "lemond",
    "lemonade",
    *[
        f"test_llamacpp_oracle_{v}"
        for v in ("linux_x64", "linux_arm64", "windows_x64", "macos_arm64")
    ],
    *[f"test_llamacpp_oracle_descriptor_{v}" for v in ("linux", "windows", "macos")],
]


def archive():
    data = subprocess.check_output(["git", "archive", BASE], cwd=REPO, timeout=60)
    return tarfile.open(fileobj=io.BytesIO(data))


def verify(source, build=None):
    with archive() as tree:
        files = [m for m in tree if m.isfile()]
        for item in files:
            expected = tree.extractfile(item).read()
            path = source / item.name
            assert (
                path.is_file() and path.read_bytes() == expected
            ), f"FrozenControlSource:{item.name}"
    assert (
        source / ".version"
    ).read_text() == CONTROL_VERSION + "\n", "FrozenControlVersionPinned"
    if build:
        verify_build_inputs(source, build)
    return len(files)


def verify_build_inputs(source, build):
    cache = (build / "CMakeCache.txt").read_text()
    assert (
        f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\n" in cache
    ), "FrozenControlOwnSourceTree"
    database = json.loads((build / "compile_commands.json").read_text())
    for row in database:
        assert (
            str(REPO / "src") not in row["command"]
        ), "FrozenControlNoProductionInputs"
        assert (
            str(REPO / "build/include") not in row["command"]
        ), "FrozenControlOwnRegistrations"
    for name, expected in GENERATED_SHA256.items():
        path = build / "include" / name
        assert path.is_file(), f"FrozenControlGenerated:{name}"
        assert (
            hashlib.sha256(path.read_bytes()).hexdigest() == expected
        ), f"FrozenControlGeneratedBytes:{name}"


def prepare(root):
    source = root / "source"
    if not source.exists():
        source.mkdir(parents=True)
        with archive() as tree:
            tree.extractall(source, filter="data")
    version = source / ".version"
    if not version.exists():
        version.write_text(CONTROL_VERSION + "\n")
    count = verify(source)
    (root / "provenance.json").write_text(
        json.dumps(
            {
                "base": BASE,
                "version": CONTROL_VERSION,
                "tree": subprocess.check_output(
                    ["git", "rev-parse", f"{BASE}^{{tree}}"], cwd=REPO, text=True
                ).strip(),
                "files": count,
            },
            indent=2,
        )
        + "\n"
    )
    return source


def build_control(root, dependencies, jobs):
    source = prepare(root)
    build = root / "build"
    subprocess.run(
        [
            "cmake",
            "-S",
            str(source),
            "-B",
            str(build),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DBUILD_WEB_APP=OFF",
            "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
            f"-DFETCHCONTENT_BASE_DIR={root / 'dependencies'}",
            *[
                f"-DFETCHCONTENT_SOURCE_DIR_{name.upper()}={dependencies / (name + '-src')}"
                for name in ("mbedtls", "httplib", "cli11", "json")
            ],
        ],
        check=True,
        timeout=120,
    )
    verify(source, build)
    subprocess.run(
        ["cmake", "--build", str(build), "--target", *TARGETS, "-j", str(jobs)],
        check=True,
        timeout=900,
    )
    print(
        f"FrozenControl: {BASE}, independent source, generated registrations, objects and links",
        flush=True,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["prepare", "build", "verify"])
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--dependencies", type=Path, default=REPO / "build/_deps")
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()
    root = args.root.resolve()
    if args.mode == "prepare":
        prepare(root)
    elif args.mode == "verify":
        print(verify(root / "source", root / "build"))
    else:
        build_control(root, args.dependencies.resolve(), args.jobs)


if __name__ == "__main__":
    main()
