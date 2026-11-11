"""Build the immutable rebased tree for activation transition replay."""

import argparse
from pathlib import Path

import container_production_control as control

BASE = "a2d90783bb635e7f1fda2ae46997ea7bb91e204e"


def build(root, dependencies):
    control.BASE = BASE
    control.build(root, dependencies)
    workspace = Path(__file__).resolve().parents[1]
    for name in (".git", ".synagent"):
        link = root / "source" / name
        if not link.exists():
            link.symlink_to(workspace / name, target_is_directory=name == ".synagent")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path, required=True)
    args = parser.parse_args()
    build(args.root.resolve(), args.dependencies.resolve())
