"""Build the unchanged C8 source for the sixteen-stage replay."""

import argparse
from pathlib import Path

import container_production_control as control

BASE = "316d3b96ccce498abc2a4a08ffd50446fc7203e2"

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path, required=True)
    args = parser.parse_args()
    control.BASE = BASE
    control.build(args.root.resolve(), args.dependencies.resolve())
