# Lemonade Unified Meson Build System

This document explains how to use the unified meson build system that can build Lemonade components (C++ and Electron) together.

## Prerequisites

### Required
- **Meson** >= 1.2.0 (`pip install meson` or system package)
- **Ninja** build system (`pip install ninja` or system package)
- **C++ compiler** (GCC, Clang, or MSVC with C++17 support)

### Optional (for Electron)
- **Node.js** >= 18 with npm
- **Electron build dependencies** (see src/app/README.md)

## Quick Start

```bash
# Configure build (from project root)
meson setup build

# Build all components
meson compile -C build

# Or use ninja directly
cd build && ninja
```

## Configuration Options

Enable/disable components during setup:

```bash
# Disable tests
meson setup build -Dtests=false
```

## Component-Specific Builds

The unified build system builds all components together. Individual component builds are not separated into specific targets.

## Development Workflows

### Electron Development

```bash
# Start Electron development server
cd build && ninja electron-dev

# Or manually:
cd src/app
npm run dev
```

### C++ Development

The C++ components are built directly as part of the unified build:

```bash
# Build C++ components (part of main build)
meson compile -C build
```

## Testing

```bash
# Run available tests
meson test -C build
```

## Installation

```bash
# Install C++ and Electron components
meson install -C build

# This will:
# - Install C++ binaries to system PATH
# - Copy Electron app to system data directory

## Packaging

The unified build system creates packages for C++ and Electron components:

```bash
# Build C++ and Electron components
meson compile -C build

# Electron packages in src/app/dist-app/
```

## Benefits of Unified Build

1. **Single Configuration**: One `meson setup` configures C++ and Electron components
2. **Dependency Management**: Automatic cross-component dependency handling for C++/Electron
3. **Parallel Builds**: C++ and Electron components build in parallel when possible
4. **Consistent Interface**: Same commands work across C++/Electron components
5. **CI/CD Friendly**: Single build system simplifies automation
6. **Cross-Platform**: Works identically on Linux, Windows, and macOS

## Comparison with Separate Builds

| Task | Separate Builds | Unified Meson |
|------|----------------|---------------|
| Configure | 3 separate commands | 1 command |
| Build All | 3 separate commands | 1 command |
| Clean All | Manual cleanup | `ninja clean` |
| Testing | 3 test suites | `meson test` |
| Packaging | Platform-specific scripts | Unified targets |

## Migration Notes

The unified system is **optional** and **backward-compatible**:

- Existing build methods (npm, pip, meson) still work
- C++ meson.build is unchanged, just orchestrated by root build
- Electron package.json scripts are preserved and called by meson

This allows gradual adoption and provides flexibility for different development needs.

## Troubleshooting

### Node.js/npm not found
```bash
# Install Node.js, then reconfigure
meson setup --reconfigure build
```

### Clean build
```bash
# Clean everything
rm -rf build/
# Or use the clean target
cd build && ninja clean-all  # if build exists
```
