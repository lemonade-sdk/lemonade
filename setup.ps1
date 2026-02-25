#!/usr/bin/env pwsh
# Lemonade development environment setup script for Windows
# This script prepares the development environment for building Lemonade

param()

$ErrorActionPreference = "Stop"

# Colors for output
$Info = "Blue"
$Success = "Green"
$Warning = "Yellow"
$Error = "Red"

# Helper functions
function Write-Info {
    param([string]$Message)
    Write-Host "[INFO] $Message" -ForegroundColor $Info
}

function Write-Success {
    param([string]$Message)
    Write-Host "[SUCCESS] $Message" -ForegroundColor $Success
}

function Write-Warning {
    param([string]$Message)
    Write-Host "[WARNING] $Message" -ForegroundColor $Warning
}

function Write-Error-Custom {
    param([string]$Message)
    Write-Host "[ERROR] $Message" -ForegroundColor $Error
}

# Check if command exists
function Command-Exists {
    param([string]$Command)

    try {
        if (Get-Command $Command -ErrorAction Stop) {
            return $true
        }
    } catch {
        return $false
    }
}

Write-Info "Lemonade Development Setup"
Write-Info "Operating System: Windows"
Write-Host ""

# Check and install pre-commit
Write-Info "Checking pre-commit installation..."

if (-not (Command-Exists "pre-commit")) {
    Write-Warning "pre-commit not found, installing..."

    if (Command-Exists "pip") {
        pip install pre-commit
    } elseif (Command-Exists "pip3") {
        pip3 install pre-commit
    } elseif (Command-Exists "py") {
        Write-Warning "Pip or Pip3 not found. Installing using py."
        py -m pip install pre-commit
        Write-Warning "If you encounter issues, please ensure the Python Scripts directory is in your PATH."
    } else {
        Write-Error-Custom "Neither pip nor pip3 found. Please install Python 3 first."
        exit 1
    }

    Write-Success "pre-commit installed"
} else {
    Write-Success "pre-commit is already installed"
}

# Install pre-commit hooks
if (Test-Path ".pre-commit-config.yaml") {
    Write-Info "Installing pre-commit hooks..."
    pre-commit install
    Write-Success "pre-commit hooks installed"
} else {
    Write-Warning "No .pre-commit-config.yaml found, skipping hook installation"
}

Write-Host ""

# Step 3: Check and install Node.js and npm
Write-Info "Step 3: Checking Node.js and npm installation..."

if (-not (Command-Exists "node")) {
    Write-Error-Custom "Node.js not found"
    Write-Info "Please install Node.js from https://nodejs.org/"
    Write-Info "You can also use Chocolatey if installed: choco install nodejs"
    exit 1
} else {
    Write-Success "Node.js is installed"
}

if (-not (Command-Exists "npm")) {
    Write-Error-Custom "npm is not available"
    Write-Info "Please reinstall Node.js or ensure npm is in your PATH"
    exit 1
} else {
    Write-Success "npm is installed"
}

Write-Host ""

# Check and install Node.js and npm
Write-Info "Checking Node.js and npm installation..."

if (-not (Command-Exists "node")) {
    Write-Error-Custom "Node.js not found"
    Write-Info "Please install Node.js from https://nodejs.org/"
    Write-Info "You can also use Chocolatey if installed: choco install nodejs"
    exit 1
} else {
    Write-Success "Node.js is installed"
}

if (-not (Command-Exists "npm")) {
    Write-Error-Custom "npm is not available"
    Write-Info "Please reinstall Node.js or ensure npm is in your PATH"
    exit 1
} else {
    Write-Success "npm is installed"
}

Write-Host ""

# Clean and create build directory
Write-Info "Preparing build directory..."

if (Test-Path "build") {
    Write-Warning "Removing existing build directory..."
    Remove-Item -Recurse -Force "build"
}

New-Item -ItemType Directory -Path "build" -Force | Out-Null
Write-Success "Build directory created"

Write-Host ""

# Configure with CMake presets
Write-Info "Configuring CMake with presets..."

cmake --preset windows
if ($LASTEXITCODE -ne 0) {
    Write-Error-Custom "CMake configuration failed"
    exit 1
}

Write-Success "CMake configured successfully"

Write-Host ""
Write-Host "==========================================" -ForegroundColor Green
Write-Success "Setup completed successfully!"
Write-Host "==========================================" -ForegroundColor Green
Write-Host ""
Write-Info "Next steps:"
Write-Host "  Build the project: cmake --build --preset windows"
Write-Host "  Build the electron app: cmake --build --preset windows --target electron-app"
Write-Host ""
Write-Info "For more information, see the README.md file"
