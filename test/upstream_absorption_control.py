"""Build the pinned C9 tree so its whole stage chain replays before the upstream absorption stage."""

import argparse
from pathlib import Path

import container_production_control as control
import llamacpp_frozen_control as frozen

BASE = "9af81abfc7435b01c676cb666d6a5c79fc2e6995"
CONTROLS = ("c8", "c8a", "c9")


def replay_layout(root, dependencies):
    workspace = Path(__file__).resolve().parents[1]
    directory = root / "source/.synagent"
    if directory.is_symlink():
        directory.unlink()
    scratch = directory / "scratch"
    scratch.mkdir(parents=True, exist_ok=True)
    for source in (workspace / ".synagent/scratch").iterdir():
        if source.name == "container-v2":
            target = scratch / source.name
            if target.is_symlink():
                target.unlink()
            for name in CONTROLS:
                (target / name).mkdir(parents=True, exist_ok=True)
                if not (target / name / "base-control").exists():
                    (target / name / "base-control").symlink_to(
                        source / name / "base-control", target_is_directory=True
                    )
        elif source.name == "backend-v2-4e3b":
            target = scratch / source.name
            target.mkdir(exist_ok=True)
            for child in source.iterdir():
                if (
                    child.name != "frozen-control"
                    and not (target / child.name).exists()
                ):
                    (target / child.name).symlink_to(
                        child, target_is_directory=child.is_dir()
                    )
        elif not (scratch / source.name).exists():
            (scratch / source.name).symlink_to(
                source, target_is_directory=source.is_dir()
            )
    repository = root / "source/.git"
    if not repository.is_symlink() and not repository.exists():
        repository.symlink_to(workspace / ".git")
    artifacts = directory / "artifacts"
    if not artifacts.is_symlink() and not artifacts.exists():
        artifacts.mkdir(parents=True)
    frozen.build_control(scratch / "backend-v2-4e3b/frozen-control", dependencies, 6)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path, required=True)
    args = parser.parse_args()
    control.BASE = BASE
    control.build(args.root.resolve(), args.dependencies.resolve())
    replay_layout(args.root.resolve(), args.dependencies.resolve())
