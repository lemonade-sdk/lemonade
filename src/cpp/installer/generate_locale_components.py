#!/usr/bin/env python3
"""
Generate WiX locale components for all .pak files in the locales directory
"""

import os
import sys
from pathlib import Path

def generate_guid(index):
    """Generate a unique GUID based on index"""
    # Generate GUIDs in a predictable pattern for locale files
    base = 0xe8f9a0b1c2d34e5f
    guid_num = base + index
    
    # Format as GUID
    p1 = (guid_num >> 96) & 0xFFFFFFFF
    p2 = (guid_num >> 80) & 0xFFFF
    p3 = (guid_num >> 64) & 0xFFFF
    p4 = (guid_num >> 48) & 0xFFFF
    p5 = guid_num & 0xFFFFFFFFFFFF
    
    return f"{p1:08x}-{p2:04x}-{p3:04x}-{p4:04x}-{p5:012x}"

def sanitize_id(filename):
    """Convert filename to valid WiX ID"""
    # Remove extension and replace hyphens with underscores
    id_name = filename.replace('.pak', '').replace('-', '_')
    # Ensure it starts with a letter
    if not id_name[0].isalpha():
        id_name = 'Locale_' + id_name
    return 'Locale' + id_name.title().replace('_', '')

def main():
    if len(sys.argv) < 2:
        print("Usage: python generate_locale_components.py <locales_directory>")
        sys.exit(1)
    
    locales_dir = Path(sys.argv[1])
    
    if not locales_dir.exists():
        print(f"Error: Directory {locales_dir} does not exist")
        sys.exit(1)
    
    # Get all .pak files
    locale_files = sorted(locales_dir.glob('*.pak'))
    
    if not locale_files:
        print(f"Warning: No .pak files found in {locales_dir}")
        return
    
    print("    <!-- Locales files (all language packs) -->")
    print("    <ComponentGroup Id=\"LocalesComponents\" Directory=\"LocalesDir\">")
    
    for idx, locale_file in enumerate(locale_files):
        filename = locale_file.name
        component_id = sanitize_id(filename)
        file_id = filename.replace('.pak', '_pak').replace('-', '_')
        guid = generate_guid(idx + 1000)
        
        print(f"      <Component Id=\"{component_id}\" Guid=\"{guid}\">")
        print(f"        <File Id=\"{file_id}\" Source=\"$(var.SourceDir)\\build\\Release\\locales\\{filename}\" KeyPath=\"yes\" />")
        print(f"      </Component>")
        print()
    
    print("    </ComponentGroup>")

if __name__ == '__main__':
    main()

