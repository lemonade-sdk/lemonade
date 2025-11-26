#!/usr/bin/env python3
"""
Generate WiX fragment for the React UI files.

This script walks the React UI build directory (src/app/dist/renderer)
and emits a WiX .wxs fragment containing component definitions for all files.
Each component receives a deterministic GUID based on the relative file path to
ensure stability across builds.

Usage:
    python generate_ui_fragment.py --source <path> --output <file.wxs>
"""

import argparse
import sys
import uuid
from pathlib import Path


def generate_deterministic_guid(rel_path: str) -> str:
    """Generate a deterministic GUID based on the file's relative path."""
    guid_value = str(uuid.uuid5(uuid.NAMESPACE_URL, f"lemonade/ui/{rel_path}")).upper()
    return f"{{{guid_value}}}"


def sanitize_id(name: str) -> str:
    """Convert a filename to a valid WiX Id (alphanumeric and underscores only)."""
    result = ""
    for c in name:
        if c.isalnum():
            result += c
        else:
            result += "_"
    # WiX IDs must start with a letter or underscore
    if result and result[0].isdigit():
        result = "_" + result
    return result


def main():
    parser = argparse.ArgumentParser(description="Generate WiX fragment for React UI files")
    parser.add_argument("--source", required=True, type=Path, help="Path to React UI directory (dist/renderer)")
    parser.add_argument("--output", required=True, type=Path, help="Destination .wxs fragment path")
    parser.add_argument("--component-group", default="UiComponents", help="ComponentGroup Id to emit")
    parser.add_argument("--directory-id", default="UiDir", help="Directory Id where UI files will be installed")
    args = parser.parse_args()

    source_dir = args.source.resolve()
    
    if not source_dir.exists():
        print(f"ERROR: UI source directory not found: {source_dir}", file=sys.stderr)
        print("Build the React UI first: cd src/app && npm run build:renderer", file=sys.stderr)
        sys.exit(1)
    
    index_html = source_dir / "index.html"
    if not index_html.exists():
        print(f"ERROR: index.html not found in {source_dir}", file=sys.stderr)
        sys.exit(1)
    
    # Collect all files
    all_files = list(source_dir.rglob("*"))
    files = [f for f in all_files if f.is_file()]
    
    if not files:
        print(f"ERROR: No files found under {source_dir}", file=sys.stderr)
        sys.exit(1)
    
    print(f"Found {len(files)} UI files to include")
    
    # Generate WiX fragment
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">',
        '  <Fragment>',
        f'    <ComponentGroup Id="{args.component_group}">',
    ]
    
    for file_path in sorted(files):
        rel_path = file_path.relative_to(source_dir).as_posix()
        component_id = f"Ui_{sanitize_id(rel_path)}"
        file_id = component_id
        guid = generate_deterministic_guid(rel_path)
        
        # Use Windows path separators for the Source attribute
        source_rel_path = rel_path.replace("/", "\\")
        
        lines.append(f'      <Component Id="{component_id}" Guid="{guid}" Directory="{args.directory_id}">')
        lines.append(f'        <File Id="{file_id}" Source="$(var.SourceDir)\\build\\Release\\resources\\ui\\{source_rel_path}" KeyPath="yes" />')
        lines.append('      </Component>')
    
    lines.append('    </ComponentGroup>')
    lines.append('  </Fragment>')
    lines.append('</Wix>')
    lines.append('')
    
    # Write output
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('\n'.join(lines), encoding='utf-8')
    
    print(f"Generated WiX fragment: {args.output}")
    print(f"  Components: {len(files)}")


if __name__ == "__main__":
    main()

