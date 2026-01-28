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

# Check and install pre-commit
print_info "Checking pre-commit installation..."
if ! command_exists pre-commit; then
    print_warning "pre-commit not found, installing..."

    if command_exists pip; then
        pip install pre-commit
    elif command_exists pip3; then
        pip3 install pre-commit
    else
        print_error "Neither pip nor pip3 found. Please install Python 3 and pip first."
        exit 1
    fi

    print_success "pre-commit installed"
else
    print_success "pre-commit is already installed"
fi

# Install pre-commit hooks
if [ -f ".pre-commit-config.yaml" ]; then
    print_info "Installing pre-commit hooks..."
    pre-commit install
    print_success "pre-commit hooks installed"
else
    print_warning "No .pre-commit-config.yaml found, skipping hook installation"
fi

echo ""

# Check required tools
print_info "Verifying required build tools..."

required_tools=("cmake" "ninja" "gcc" "g++")
missing_tools=()

for tool in "${required_tools[@]}"; do
    if command_exists "$tool"; then
        version=$($tool --version 2>&1 | head -1)
        print_success "$tool is installed"
    else
        print_warning "$tool not found"
        missing_tools+=("$tool")
    fi
done

if [ ${#missing_tools[@]} -gt 0 ]; then
    print_error "Missing required tools: ${missing_tools[*]}"
    print_info "Please install them using your package manager:"

    if [ "$OS" = "linux" ]; then
        if command_exists apt-get; then
            print_info "Ubuntu/Debian: sudo apt-get install cmake ninja-build build-essential"
        elif command_exists pacman; then
            print_info "Arch: sudo pacman -S cmake ninja base-devel"
        elif command_exists dnf; then
            print_info "Fedora: sudo dnf install cmake ninja-build gcc gcc-c++ make"
        fi
    elif [ "$OS" = "macos" ]; then
        print_info "macOS: brew install cmake ninja"
    fi

    exit 1
fi

echo ""

# Check and install Node.js and npm
print_info "Checking Node.js and npm installation..."

if ! command_exists node; then
    print_warning "Node.js not found, installing..."

    if [ "$OS" = "linux" ]; then
        if command_exists apt-get; then
            sudo apt-get update
            sudo apt-get install -y nodejs npm
        elif command_exists pacman; then
            sudo pacman -S nodejs npm
        elif command_exists dnf; then
            sudo dnf install -y nodejs npm
        else
            print_error "Unable to install Node.js automatically on this Linux distribution"
            print_info "Please install Node.js from https://nodejs.org/"
            exit 1
        fi
    elif [ "$OS" = "macos" ]; then
        if command_exists brew; then
            brew install node
        else
            print_error "brew not found. Please install Homebrew first: https://brew.sh/"
            exit 1
        fi
    fi

    print_success "Node.js installed"
else
    print_success "Node.js is already installed"
fi

if ! command_exists npm; then
    print_error "npm is still not available after Node.js installation"
    exit 1
else
    print_success "npm is available"
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

# Install npm dependencies for Electron app
print_info "Installing Electron app dependencies..."

if [ -d "src/app" ]; then
    print_info "Installing npm dependencies in src/app..."
    cd src/app

    if [ ! -d "node_modules" ]; then
        npm install
        print_success "npm dependencies installed"
    else
        print_success "npm dependencies already installed"
    fi

    cd ../..
else
    print_warning "src/app directory not found, skipping Electron app setup"
fi

echo ""
echo "=========================================="
print_success "Setup completed successfully!"
echo "=========================================="
echo ""
print_info "Next steps:"
echo "  Build the project: cmake --build build"
echo ""
print_info "For more information, see the README.md file"
