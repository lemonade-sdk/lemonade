#!/usr/bin/env pwsh
# Lemonade development environment setup script for Windows
# This script prepares the development environment for building Lemonade

param()

$ErrorActionPreference = "Stop"

# Colors for output
$Info = "Blue"
$Success = "Green"
$Warning = "Yellow"
$ErrorColor = "Red"

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
    Write-Host "[ERROR] $Message" -ForegroundColor $ErrorColor
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
if (-not (Command-Exists "cargo") -or -not (Command-Exists "rustc")) {
    $rustNeedsInstall = $true
    Write-Info "Rust toolchain not found"
} else {
    Write-Success "Rust toolchain is installed"
}

if ($rustNeedsInstall) {
    Write-Info "Installing Rust toolchain automatically..."
    $rustInstalled = $false

    if (Command-Exists "winget") {
        Write-Info "Trying Windows package manager install first..."
        & winget install --exact --id Rustlang.Rustup --accept-package-agreements --accept-source-agreements --disable-interactivity --scope user
        if ($LASTEXITCODE -eq 0) {
            $machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
            $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
            $env:PATH = "$userPath;$machinePath;$env:PATH"

            if (Command-Exists "rustup") {
                & rustup default stable | Out-Null
                $cargoBin = Join-Path $env:USERPROFILE ".cargo\bin"
                if (Test-Path (Join-Path $cargoBin "cargo.exe")) {
                    $env:PATH = "$cargoBin;$env:PATH"
                }
                if ((Command-Exists "cargo") -and (Command-Exists "rustc")) {
                    $rustInstalled = $true
                }
            }
        } else {
            Write-Warning "winget Rust install failed (exit code $LASTEXITCODE)"
        }
    } else {
        Write-Warning "winget not available, skipping package-manager Rust install"
    }

    if (-not $rustInstalled) {
        Write-Warning "Falling back to rustup-init.exe download..."
        $rustupInit = Join-Path $env:TEMP "rustup-init.exe"
        $downloadOk = $true
        try {
            Invoke-WebRequest -UseBasicParsing -Uri "https://win.rustup.rs/x86_64" -OutFile $rustupInit
        } catch {
            Write-Error-Custom "Failed to download rustup-init.exe: $_"
            exit 1
        }

        if ($downloadOk) {
            & $rustupInit -y --default-toolchain stable --no-modify-path | Out-Null
            if ($LASTEXITCODE -ne 0) {
                Write-Error-Custom "rustup install failed (exit code $LASTEXITCODE)"
                exit 1
            } else {
                $cargoBin = Join-Path $env:USERPROFILE ".cargo\bin"
                $env:PATH = "$cargoBin;$env:PATH"

                if ((Command-Exists "cargo") -and (Command-Exists "rustc")) {
                    $rustInstalled = $true
                }
            }
        }
    }

    if (-not $rustInstalled) {
        Write-Error-Custom "Rust installation completed, but cargo/rustc are still unavailable"
        exit 1
    }

    Write-Success "Rust toolchain installed"
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

Write-Info "Next steps:"
Write-Host "  Build the project: cmake --build --preset windows"
Write-Host "  Build the Tauri desktop app: cmake --build --preset windows --target tauri-app"
Write-Host "    (first build downloads ~80 Rust crates and may take several minutes)"
Write-Host "  Hot-reload the desktop UI during development: cd src/app; npm run dev"
Write-Host ""
Write-Info "For more information, see the README.md file"
