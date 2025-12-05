#!/usr/bin/env python3
"""
Generate a WiX fragment that packages every file from the Electron app directory.

This script walks the unpacked Electron app (e.g. electron-builder's win-unpacked)
and produces a WiX source fragment with:

1. Directory structure rooted at a caller-provided Directory Id (e.g. ElectronAppDir)
2. A ComponentGroup that installs every file under that directory.

Each component receives a deterministic GUID based on the relative file path to
ensure stability across builds.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys
import textwrap
import uuid


MAX_WIX_ID_LENGTH = 70


def make_safe_id(prefix: str, rel_path: str) -> str:
    """Create a WiX-safe identifier with a deterministic hash suffix."""
    sanitized = []
    for ch in rel_path:
        if ch.isalnum():
            sanitized.append(ch)
        else:
            sanitized.append("_")
    safe = "".join(sanitized).strip("_")
    if not safe:
        safe = "root"
    if not safe[0].isalpha():
        safe = f"_{safe}"
    hash_suffix = hashlib.sha1(rel_path.encode("utf-8")).hexdigest()[:8]
    max_body = MAX_WIX_ID_LENGTH - len(prefix) - len(hash_suffix) - 2  # underscores
    if max_body < 1:
        raise ValueError("Prefix is too long to form a valid WiX identifier.")
    if len(safe) > max_body:
        safe = safe[:max_body]
    return f"{prefix}_{safe}_{hash_suffix}"


class DirNode:
    def __init__(
        self, rel_path: Path, dir_id: str, name: str | None, parent: "DirNode | None"
    ):
        self.rel_path = rel_path
        self.id = dir_id
        self.name = name
        self.parent = parent
        self.children: dict[str, DirNode] = {}


def ensure_directory_nodes(
    root: DirNode, rel_path: Path, nodes_by_rel: dict[str, DirNode]
) -> DirNode:
    """Ensure DirNode objects exist for every component of rel_path."""
    rel_str = rel_path.as_posix()
    if rel_str in nodes_by_rel:
        return nodes_by_rel[rel_str]

    parent = (
        ensure_directory_nodes(root, rel_path.parent, nodes_by_rel)
        if rel_path.parts
        else root
    )
    dir_id = make_safe_id("ElectronDir", rel_str or "root")
    node = DirNode(rel_path, dir_id, rel_path.name or None, parent)
    parent.children[rel_path.name] = node
    nodes_by_rel[rel_str or "."] = node
    return node


def render_directory_xml(node: DirNode, indent: str = "      ") -> list[str]:
    """Recursively build XML lines for Directory hierarchy (excluding root)."""
    lines: list[str] = []
    for child_name in sorted(node.children):
        child = node.children[child_name]
        lines.append(f'{indent}<Directory Id="{child.id}" Name="{child.name}">')
        lines.extend(render_directory_xml(child, indent + "  "))
        lines.append(f"{indent}</Directory>")
    return lines


def generate_wxs(
    source_dir: Path,
    output_path: Path,
    component_group: str,
    root_id: str,
    path_variable: str,
) -> None:
    if not source_dir.exists():
        raise FileNotFoundError(
            f"Electron app directory not found: {source_dir}\n"
            "Run the electron-app target (npm run build:win) before building the installer."
        )

    files = sorted(p for p in source_dir.rglob("*") if p.is_file())
    if not files:
        raise FileNotFoundError(
            f"No files found under {source_dir}. Did the Electron build complete successfully?"
        )

    root_node = DirNode(Path("."), root_id, None, None)
    nodes_by_rel: dict[str, DirNode] = {".": root_node}

    for file_path in files:
        rel_dir = file_path.relative_to(source_dir).parent
        ensure_directory_nodes(root_node, rel_dir, nodes_by_rel)

    directory_lines = render_directory_xml(root_node)

    file_entries = []
    for file_path in files:
        rel_path = file_path.relative_to(source_dir).as_posix()
        rel_dir = file_path.relative_to(source_dir).parent.as_posix() or "."
        dir_node = nodes_by_rel[rel_dir]
        component_id = make_safe_id("ElectronComponent", rel_path)
        file_id = make_safe_id("ElectronFile", rel_path)
        guid_value = str(
            uuid.uuid5(uuid.NAMESPACE_URL, f"lemonade/electron/{rel_path}")
        ).upper()
        guid = f"{{{guid_value}}}"
        windows_rel_path = rel_path.replace("/", "\\")
        file_entries.append(
            textwrap.dedent(
                f"""\
                <Component Id="{component_id}" Guid="{guid}" Directory="{dir_node.id}">
                  <File Id="{file_id}"
                        Source="$(var.{path_variable})\\{windows_rel_path}"
                        KeyPath="yes" />
                </Component>"""
            ).rstrip()
        )

    content = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">',
        "  <Fragment>",
        f'    <DirectoryRef Id="{root_id}">',
    ]
    if directory_lines:
        content.extend(directory_lines)
    content.append("    </DirectoryRef>")
    content.append("  </Fragment>")
    content.append("")
    content.append("  <Fragment>")
    content.append(f'    <ComponentGroup Id="{component_group}">')
    for entry in file_entries:
        for line in entry.splitlines():
            content.append(f"      {line}")
    content.append("    </ComponentGroup>")
    content.append("  </Fragment>")
    content.append("</Wix>")
    content.append("")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(content), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate WiX fragment for Electron app files."
    )
    parser.add_argument(
        "--source",
        required=True,
        type=Path,
        help="Path to Electron win-unpacked directory.",
    )
    parser.add_argument(
        "--output", required=True, type=Path, help="Destination .wxs fragment path."
    )
    parser.add_argument(
        "--component-group", required=True, help="ComponentGroup Id to emit."
    )
    parser.add_argument(
        "--root-id",
        required=True,
        help="Directory Id where the Electron app root will be installed.",
    )
    parser.add_argument(
        "--path-variable",
        default="ElectronSourceDir",
        help="WiX preprocessor variable resolving to the Electron source directory.",
    )
    args = parser.parse_args()

    try:
        generate_wxs(
            args.source.resolve(),
            args.output.resolve(),
            args.component_group,
            args.root_id,
            args.path_variable,
        )
    except Exception as exc:  # pylint: disable=broad-except
        print(f"[generate_electron_fragment] ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
