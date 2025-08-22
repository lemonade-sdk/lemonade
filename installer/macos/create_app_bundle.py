#!/usr/bin/env python3
"""
Script to create a proper macOS .app bundle for Lemonade Server
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

def create_app_bundle(output_dir: str, python_version: str = "3.12"):
    """Create a Lemonade Server.app bundle"""
    
    app_name = "Lemonade Server"
    bundle_name = f"{app_name}.app"
    bundle_path = Path(output_dir) / bundle_name
    
    print(f"Creating {bundle_name} at {bundle_path}")
    
    # Remove existing bundle if it exists
    if bundle_path.exists():
        shutil.rmtree(bundle_path)
    
    # Create bundle directory structure
    contents_dir = bundle_path / "Contents"
    macos_dir = contents_dir / "MacOS"
    resources_dir = contents_dir / "Resources"
    frameworks_dir = contents_dir / "Frameworks"
    
    # Create directories
    macos_dir.mkdir(parents=True, exist_ok=True)
    resources_dir.mkdir(parents=True, exist_ok=True)
    frameworks_dir.mkdir(parents=True, exist_ok=True)
    
    # Create Info.plist
    info_plist_content = f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>{app_name}</string>
    <key>CFBundleDisplayName</key>
    <string>{app_name}</string>
    <key>CFBundleIdentifier</key>
    <string>ai.lemonade-server.app</string>
    <key>CFBundleVersion</key>
    <string>1.0.0</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0.0</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleSignature</key>
    <string>????</string>
    <key>CFBundleExecutable</key>
    <string>lemonade-server</string>
    <key>CFBundleIconFile</key>
    <string>icon.icns</string>
    <key>LSMinimumSystemVersion</key>
    <string>11.0</string>
    <key>LSUIElement</key>
    <true/>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSRequiresAquaSystemAppearance</key>
    <false/>
    <key>NSSupportsAutomaticGraphicsSwitching</key>
    <true/>
</dict>
</plist>'''
    
    # Write Info.plist
    with open(contents_dir / "Info.plist", "w") as f:
        f.write(info_plist_content)
    
    # Create the main executable script
    executable_script = f'''#!/bin/bash

# Get the directory where this script is located (inside the .app bundle)
SCRIPT_DIR="$(cd "$(dirname "${{BASH_SOURCE[0]}}")" && pwd)"
BUNDLE_DIR="$(dirname "$SCRIPT_DIR")"
RESOURCES_DIR="$BUNDLE_DIR/Resources"

# Add the embedded Python to PATH
export PATH="$RESOURCES_DIR/python/bin:$PATH"
export PYTHONPATH="$RESOURCES_DIR/python/lib/python{python_version}/site-packages:$PYTHONPATH"

# Set up environment for Lemonade Server
export LEMONADE_SERVER_APP_BUNDLE="1"

# Change to Resources directory
cd "$RESOURCES_DIR"

# Start Lemonade Server with tray
exec "$RESOURCES_DIR/python/bin/python{python_version}" -m lemonade.tools.server.tray
'''
    
    # Write the executable script
    executable_path = macos_dir / "lemonade-server"
    with open(executable_path, "w") as f:
        f.write(executable_script)
    
    # Make the executable script executable
    os.chmod(executable_path, 0o755)
    
    # Copy favicon as app icon (we'll need to convert it to .icns format)
    favicon_path = Path(__file__).parent.parent.parent / "src" / "lemonade" / "tools" / "server" / "static" / "favicon.ico"
    if favicon_path.exists():
        # For now, just copy the favicon - in production we'd convert it to .icns
        shutil.copy2(favicon_path, resources_dir / "icon.ico")
        print("Note: Using .ico file as placeholder. In production, convert to .icns format.")
    
    print(f"Created {bundle_name} successfully!")
    return bundle_path

def embed_python_in_bundle(bundle_path: Path, python_version: str = "3.12"):
    """Embed a Python runtime in the app bundle"""
    
    resources_dir = bundle_path / "Contents" / "Resources"
    python_dir = resources_dir / "python"
    
    print("Setting up embedded Python runtime...")
    
    # Create Python directory
    python_dir.mkdir(exist_ok=True)
    
    # For now, we'll use the system Python and pip install into the bundle
    # In production, you'd want to download and embed a standalone Python
    
    # Install Lemonade Server and dependencies into the bundle
    install_cmd = [
        sys.executable, "-m", "pip", "install", 
        "--target", str(python_dir / "lib" / f"python{python_version}" / "site-packages"),
        "lemonade-sdk[oga-cpu]", "rumps"
    ]
    
    print(f"Installing packages: {' '.join(install_cmd)}")
    result = subprocess.run(install_cmd, capture_output=True, text=True)
    
    if result.returncode != 0:
        print(f"Error installing packages: {result.stderr}")
        return False
    
    # Create bin directory and symlinks
    bin_dir = python_dir / "bin"
    bin_dir.mkdir(exist_ok=True)
    
    # Create a simple Python launcher
    python_launcher = f'''#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${{BASH_SOURCE[0]}}")" && pwd)"
PYTHON_DIR="$(dirname "$SCRIPT_DIR")"
export PYTHONPATH="$PYTHON_DIR/lib/python{python_version}/site-packages:$PYTHONPATH"
exec {sys.executable} "$@"
'''
    
    launcher_path = bin_dir / f"python{python_version}"
    with open(launcher_path, "w") as f:
        f.write(python_launcher)
    os.chmod(launcher_path, 0o755)
    
    print("Embedded Python runtime setup complete!")
    return True

def create_dmg(app_bundle_path: Path, output_dir: str):
    """Create a .dmg disk image containing the app bundle"""
    
    app_name = "Lemonade Server"
    dmg_name = f"{app_name.replace(' ', '-')}.dmg"
    dmg_path = Path(output_dir) / dmg_name
    
    print(f"Creating {dmg_name}...")
    
    # Remove existing DMG if it exists
    if dmg_path.exists():
        dmg_path.unlink()
    
    # Create temporary directory for DMG contents
    with tempfile.TemporaryDirectory() as temp_dir:
        temp_path = Path(temp_dir)
        
        # Copy app bundle to temp directory
        temp_app_path = temp_path / app_bundle_path.name
        shutil.copytree(app_bundle_path, temp_app_path)
        
        # Create Applications symlink
        applications_link = temp_path / "Applications"
        os.symlink("/Applications", applications_link)
        
        # Create the DMG
        create_dmg_cmd = [
            "hdiutil", "create",
            "-format", "UDZO",
            "-srcfolder", str(temp_path),
            "-volname", app_name,
            str(dmg_path)
        ]
        
        print(f"Running: {' '.join(create_dmg_cmd)}")
        result = subprocess.run(create_dmg_cmd, capture_output=True, text=True)
        
        if result.returncode != 0:
            print(f"Error creating DMG: {result.stderr}")
            return False
        
        print(f"Successfully created {dmg_path}")
        return dmg_path

def main():
    if len(sys.argv) < 2:
        print("Usage: python create_app_bundle.py <output_directory> [python_version]")
        sys.exit(1)
    
    output_dir = sys.argv[1]
    python_version = sys.argv[2] if len(sys.argv) > 2 else "3.12"
    
    # Ensure output directory exists
    Path(output_dir).mkdir(parents=True, exist_ok=True)
    
    # Create the app bundle
    bundle_path = create_app_bundle(output_dir, python_version)
    
    # Embed Python in the bundle
    if not embed_python_in_bundle(bundle_path, python_version):
        print("Failed to embed Python runtime")
        sys.exit(1)
    
    # Create DMG
    dmg_path = create_dmg(bundle_path, output_dir)
    if not dmg_path:
        print("Failed to create DMG")
        sys.exit(1)
    
    print(f"\\nMacOS installer created successfully!")
    print(f"App Bundle: {bundle_path}")
    print(f"DMG Image: {dmg_path}")

if __name__ == "__main__":
    main()
