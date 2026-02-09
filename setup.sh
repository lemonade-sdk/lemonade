#!/bin/bash
# Lemonade development environment setup script
# This script prepares the development environment for building Lemonade
set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Helper functions
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Check if running as root
is_root() {
    [ "$(id -u)" -eq 0 ]
}

# Use sudo only if not root
maybe_sudo() {
    if is_root; then
        "$@"
    else
        sudo "$@"
    fi
}

# Detect OS
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    OS="linux"
elif [[ "$OSTYPE" == "darwin"* ]]; then
    OS="macos"
else
    print_error "Unsupported OS: $OSTYPE"
    exit 1
fi

print_info "Lemonade Development Setup"
print_info "Operating System: $OS"
echo ""

# Arrays to track missing dependencies
missing_packages=()
install_commands=()

# Check pre-commit
print_info "Checking pre-commit installation..."
if ! command_exists pre-commit; then
    print_warning "pre-commit not found"
    if [ "$OS" = "linux" ] && command_exists pacman; then
        missing_packages+=("pre-commit")
    elif [ "$OS" = "linux" ] && command_exists apt; then
        missing_packages+=("pre-commit")
    elif [ "$OS" = "linux" ] && command_exists dnf; then
        missing_packages+=("pre-commit")
    elif [ "$OS" = "macos" ] && command_exists brew; then
        missing_packages+=("pre-commit")
    elif command_exists pip || command_exists pip3; then
        missing_packages+=("pre-commit (via pip)")
    else
        print_error "No package manager found to install pre-commit"
        exit 1
    fi
else
    print_success "pre-commit is already installed"
fi

# Check required build tools
print_info "Checking required build tools..."

required_tools=("git" "cmake" "ninja" "gcc" "g++")
missing_tools=()

for tool in "${required_tools[@]}"; do
    if command_exists "$tool"; then
        print_success "$tool is installed"
    else
        print_warning "$tool not found"
        missing_tools+=("$tool")
    fi
done

if [ ${#missing_tools[@]} -gt 0 ]; then
    if [ "$OS" = "linux" ]; then
        if command_exists apt; then
            missing_packages+=("git" "cmake" "ninja-build" "build-essential")
        elif command_exists pacman; then
            missing_packages+=("git" "cmake" "ninja" "base-devel")
        elif command_exists dnf; then
            missing_packages+=("git" "cmake" "ninja-build" "gcc" "gcc-c++" "make")
        fi
    elif [ "$OS" = "macos" ]; then
        missing_packages+=("git" "cmake" "ninja")
    fi
fi

# Check pkg-config and required libraries
print_info "Checking pkg-config and required libraries..."

if command_exists pkg-config; then
    print_success "pkg-config is installed"

    # Check for required libraries using pkg-config
    libs_to_check=("libcurl" "openssl" "zlib" "libsystemd")
    missing_libs=()

    for lib in "${libs_to_check[@]}"; do
        if pkg-config --exists "$lib" 2>/dev/null; then
            print_success "$lib is installed"
        else
            print_warning "$lib not found"
            missing_libs+=("$lib")
        fi
    done

    if [ ${#missing_libs[@]} -gt 0 ]; then
        if [ "$OS" = "linux" ]; then
            if command_exists apt; then
                missing_packages+=("libcurl4-openssl-dev" "libssl-dev" "zlib1g-dev" "libsystemd-dev")
            elif command_exists pacman; then
                missing_packages+=("curl" "openssl" "zlib" "systemd")
            elif command_exists dnf; then
                missing_packages+=("libcurl-devel" "openssl-devel" "zlib-devel" "systemd-devel")
            fi
        elif [ "$OS" = "macos" ]; then
            # macOS typically has these via Xcode Command Line Tools
            # but can be explicitly installed via brew if needed
            for lib in "${missing_libs[@]}"; do
                case "$lib" in
                    libcurl) missing_packages+=("curl") ;;
                    openssl) missing_packages+=("openssl") ;;
                    zlib) missing_packages+=("zlib") ;;
                esac
            done
        fi
    fi
else
    print_warning "pkg-config not found, assuming libraries need to be installed"
    if [ "$OS" = "linux" ]; then
        if command_exists apt; then
            missing_packages+=("pkg-config" "libcurl4-openssl-dev" "libssl-dev" "zlib1g-dev" "libsystemd-dev")
        elif command_exists pacman; then
            missing_packages+=("pkgconf" "curl" "openssl" "zlib" "systemd")
        elif command_exists dnf; then
            missing_packages+=("pkgconfig" "libcurl-devel" "openssl-devel" "zlib-devel" "systemd-devel")
        fi
    elif [ "$OS" = "macos" ]; then
        missing_packages+=("pkg-config" "curl" "openssl" "zlib")
    fi
fi

# Check Node.js and npm
print_info "Checking Node.js and npm installation..."

if ! command_exists node; then
    print_warning "Node.js not found"
    if [ "$OS" = "linux" ]; then
        if command_exists apt || command_exists pacman || command_exists dnf; then
            missing_packages+=("nodejs" "npm")
        fi
    elif [ "$OS" = "macos" ]; then
        if command_exists brew; then
            missing_packages+=("node")
        else
            print_error "Homebrew not found. Please install Homebrew first: https://brew.sh/"
            exit 1
        fi
    fi
else
    print_success "Node.js is already installed"
fi

if command_exists node && ! command_exists npm; then
    print_warning "npm not found"
    missing_packages+=("npm")
fi

# Check for KaTeX fonts (optional but recommended for packaging)
print_info "Checking KaTeX fonts installation..."
if [ "$OS" = "linux" ]; then
    KATEX_FONTS_DIR="/usr/share/fonts/truetype/katex"
    if [ -d "$KATEX_FONTS_DIR" ]; then
        print_success "KaTeX fonts are already installed"
    else
        print_warning "KaTeX fonts not found (optional, enables symlinks in packages)"
        if command_exists apt; then
            missing_packages+=("fonts-katex")
        fi
    fi
fi

echo ""

# If there are missing packages, prompt for installation
if [ ${#missing_packages[@]} -gt 0 ]; then
    print_warning "Missing dependencies detected:"
    for pkg in "${missing_packages[@]}"; do
        echo "  - $pkg"
    done
    echo ""

    # Build installation command
    if [ "$OS" = "linux" ]; then
        if command_exists apt; then
            if is_root; then
                install_cmd="apt update && apt install -y ${missing_packages[*]}"
            else
                install_cmd="sudo apt update && sudo apt install -y ${missing_packages[*]}"
            fi
        elif command_exists pacman; then
            if is_root; then
                install_cmd="pacman -Syu --needed --noconfirm ${missing_packages[*]}"
            else
                install_cmd="sudo pacman -Syu --needed --noconfirm ${missing_packages[*]}"
            fi
        elif command_exists dnf; then
            if is_root; then
                install_cmd="dnf install -y ${missing_packages[*]}"
            else
                install_cmd="sudo dnf install -y ${missing_packages[*]}"
            fi
        fi
    elif [ "$OS" = "macos" ]; then
        install_cmd="brew install ${missing_packages[*]}"
    fi

    print_info "The following command will be executed:"
    echo "  $install_cmd"
    echo ""

    # Check if running in CI environment
    if [ -n "$CI" ] || [ -n "$GITHUB_ACTIONS" ]; then
        print_info "CI environment detected, proceeding with automatic installation..."
    else
        read -p "Do you want to install these dependencies? (y/N): " -n 1 -r
        echo ""

        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            print_error "Installation aborted by user"
            exit 1
        fi
    fi

    print_info "Installing dependencies..."

    # Install based on OS and package manager
    if [ "$OS" = "linux" ]; then
        if command_exists apt; then
            maybe_sudo apt update
            # Check if pre-commit needs pip
            if [[ " ${missing_packages[*]} " =~ " pre-commit " ]]; then
                maybe_sudo apt install -y pre-commit
            fi
            # Install other packages
            other_packages=("${missing_packages[@]}")
            other_packages=("${other_packages[@]/pre-commit/}")
            if [ ${#other_packages[@]} -gt 0 ]; then
                maybe_sudo apt install -y ${other_packages[@]}
            fi
        elif command_exists pacman; then
            maybe_sudo pacman -Syu --needed --noconfirm ${missing_packages[@]}
        elif command_exists dnf; then
            maybe_sudo dnf install -y ${missing_packages[@]}
        fi
    elif [ "$OS" = "macos" ]; then
        brew install ${missing_packages[@]}
    fi

    # Install pre-commit via pip if no package manager version available
    if [[ " ${missing_packages[*]} " =~ " pre-commit (via pip) " ]]; then
        if command_exists pip; then
            pip install pre-commit
        elif command_exists pip3; then
            pip3 install pre-commit
        fi
    fi

    print_success "Dependencies installed successfully"
    echo ""
else
    print_success "All dependencies are already installed"
    echo ""
fi

# Install pre-commit hooks
if [ -f ".pre-commit-config.yaml" ] && [ ! -f ".git/hooks/pre-commit" ] && [ -z "$CI" ] && [ -z "$GITHUB_ACTIONS" ]; then
    print_info "Installing pre-commit hooks..."
    pre-commit install
    print_success "pre-commit hooks installed"
fi

echo ""

# Clean and create build directory
print_info "Preparing build directory..."

if [ -d "build" ]; then
    print_warning "Removing existing build directory..."
    rm -rf build
fi

mkdir -p build
print_success "Build directory created"

echo ""

# Configure with CMake presets
print_info "Configuring CMake with presets..."

cmake --preset default

print_success "CMake configured successfully"

echo ""
echo "=========================================="
print_success "Setup completed successfully!"
echo "=========================================="
echo ""
print_info "Next steps:"
echo "  Build the project: cmake --build --preset default"
echo "  Build the electron app: cmake --build --preset default --target electron-app"
echo ""
print_info "For more information, see the docs/dev-getting-started.md file"
