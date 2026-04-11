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

# Check Rust toolchain (OPTIONAL — only needed for the Tauri desktop app).
# Like setup.sh, this is split into a "detect" pass and an "install" pass
# gated on a y/N prompt. CI mode skips the install by default; opt in via
# LEMONADE_SETUP_TAURI=1. A failure to install Rust does NOT abort the
# script — the C++ server build doesn't depend on it.
Write-Info "Checking Rust toolchain installation..."

# rustup may have installed cargo into ~/.cargo/bin without it being on PATH
# yet for this PowerShell session (the installer updates the user PATH but
# existing shells don't pick that up until they restart).
if (-not (Command-Exists "cargo") -or -not (Command-Exists "rustc")) {
    $cargoBin = Join-Path $env:USERPROFILE ".cargo\bin"
    if (Test-Path (Join-Path $cargoBin "cargo.exe")) {
        $env:PATH = "$cargoBin;$env:PATH"
    }
}

$rustNeedsInstall = $false
$rustWasJustInstalled = $false
if (-not (Command-Exists "cargo") -or -not (Command-Exists "rustc")) {
    $rustNeedsInstall = $true
    Write-Info "Rust toolchain not found (optional — only required for the Tauri desktop app)"
} else {
    Write-Success "Rust toolchain is installed"
}

if ($rustNeedsInstall) {
    Write-Host ""
    Write-Info "Optional Tauri desktop-app dependency:"
    Write-Host "  - Rust toolchain (via rustup)"
    Write-Info "This is ONLY needed if you want to build the Tauri desktop app"
    Write-Info "(cmake --build --target tauri-app). The C++ server build does NOT need it."
    Write-Host ""

    $installRust = $false
    if ($env:CI -or $env:GITHUB_ACTIONS) {
        if ($env:LEMONADE_SETUP_TAURI -eq "1") {
            Write-Info "LEMONADE_SETUP_TAURI=1 detected, installing Rust in CI..."
            $installRust = $true
        } else {
            Write-Info "CI environment detected, skipping optional Rust install."
            Write-Info "Set LEMONADE_SETUP_TAURI=1 to enable in CI."
        }
    } else {
        $reply = Read-Host "Install Rust via rustup now? (y/N)"
        if ($reply -match '^[Yy]$') {
            $installRust = $true
        }
    }

    if ($installRust) {
        $rustupInit = Join-Path $env:TEMP "rustup-init.exe"
        Write-Info "Downloading rustup-init.exe..."
        $downloadOk = $true
        try {
            Invoke-WebRequest -UseBasicParsing -Uri "https://win.rustup.rs/x86_64" -OutFile $rustupInit
        } catch {
            Write-Warning "Failed to download rustup-init.exe: $_"
            Write-Info "Install Rust manually from https://rustup.rs if you need the Tauri build."
            $downloadOk = $false
        }

        if ($downloadOk) {
            Write-Info "Running rustup-init.exe..."
            & $rustupInit -y --default-toolchain stable --no-modify-path | Out-Null
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "rustup install failed (exit code $LASTEXITCODE)"
                Write-Info "Install Rust manually from https://rustup.rs if you need the Tauri build."
            } else {
                # Add cargo to PATH for the rest of this script's session.
                $cargoBin = Join-Path $env:USERPROFILE ".cargo\bin"
                $env:PATH = "$cargoBin;$env:PATH"

                if (Command-Exists "cargo") {
                    Write-Success "Rust toolchain installed"
                    $rustWasJustInstalled = $true
                } else {
                    Write-Warning "Rust install reported success but cargo is still not on PATH"
                    Write-Info "Open a new shell if you need cargo for the Tauri build."
                }
            }
        }
    }
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

# Detect Visual Studio version and select CMake preset
$vswhereExe = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$cmakePreset = "windows"

if (Test-Path $vswhereExe) {
    $vsMajor = (& $vswhereExe -latest -property catalog_productLineVersion)
    if ($vsMajor -eq "18") {
        $cmakePreset = "vs18"
    }
    Write-Info "Detected Visual Studio v$vsMajor, using preset: $cmakePreset"
} else {
    Write-Warning "vswhere not found, defaulting to preset: windows"
}

cmake --preset $cmakePreset
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

# If we just installed Rust in this run, remind the user that their EXISTING
# PowerShell sessions don't have cargo on PATH yet — rustup updates the user
# PATH but only new shells pick it up.
if ($rustWasJustInstalled) {
    Write-Warning "Rust was just installed."
    Write-Info "To use cargo in your CURRENT PowerShell session, restart it,"
    Write-Info "or prepend `$HOME\.cargo\bin to `$env:PATH manually."
    Write-Info "New shells will pick it up automatically."
    Write-Host ""
}

Write-Info "Next steps:"
Write-Host "  Build the project: cmake --build --preset windows"
Write-Host "  Build the Tauri desktop app: cmake --build --preset windows --target tauri-app"
Write-Host "    (first build downloads ~80 Rust crates and may take several minutes)"
Write-Host "  Hot-reload the desktop UI during development: cd src/app; npm run dev"
Write-Host ""
Write-Info "For more information, see the README.md file"
