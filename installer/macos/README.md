# macOS Installer for Lemonade Server

This directory contains the macOS installer creation tools for Lemonade Server.

## Overview

The macOS installer creates a proper native `.dmg` disk image containing a `.app` bundle, following macOS conventions. This is a significant improvement over the previous shell script approach.

## Files

- `create_app_bundle.py` - Python script that creates the `.app` bundle and `.dmg` installer
- `README.md` - This file

## How it Works

The installer creation process:

1. **Creates App Bundle Structure**: Builds a standard macOS `.app` bundle with:
   - `Contents/Info.plist` - App metadata and configuration
   - `Contents/MacOS/lemonade-server` - Main executable launcher script
   - `Contents/Resources/` - Python runtime and dependencies
   - `Contents/Frameworks/` - Reserved for future use

2. **Embeds Python Runtime**: Installs Lemonade Server and dependencies into the app bundle so it's completely self-contained

3. **Creates DMG**: Packages the app bundle into a `.dmg` disk image with:
   - The Lemonade Server.app bundle
   - A symlink to /Applications for easy installation

## Installation for Users

Users can install Lemonade Server by:

1. Opening the `.dmg` file
2. Dragging "Lemonade Server.app" to the "Applications" folder
3. Running the app from Applications or Launchpad

The app will appear in the menu bar with a tray icon for managing the server.

## Comparison to Windows Installer

| Feature | Windows (.exe) | macOS (.dmg) |
|---------|---------------|---------------|
| Installer Format | NSIS executable | Disk image |
| Installation | Guided wizard | Drag & drop |
| App Location | Custom directory | Applications folder |
| System Integration | Registry, PATH | App bundle, menu bar |
| Dependencies | Embedded Python | Embedded Python |
| Uninstall | Control Panel | Move to Trash |

## Development

To test the installer locally:

```bash
# Create a test installer
python installer/macos/create_app_bundle.py /tmp/test_installer 3.12

# Check the results
ls -la /tmp/test_installer/
open /tmp/test_installer/Lemonade-Server.dmg
```

## CI/CD Integration

The GitHub Actions workflow (`.github/workflows/server_installer_macos_latest.yml`) automatically:

1. Builds the app bundle and DMG
2. Tests the installer structure
3. Uploads artifacts
4. Creates releases with the DMG file

## Future Improvements

- Convert favicon.ico to proper .icns icon format
- Add code signing for distribution outside App Store
- Consider App Store distribution
- Add more sophisticated Python runtime embedding
- Implement auto-updater integration
