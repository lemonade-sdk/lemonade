# Build WiX MSI Installer for Lemonade Server
# This script builds the MSI installer using WiX Toolset 5.0.2

param(
    [string]$Configuration = "Release",
    [switch]$Help
)

if ($Help) {
    Write-Host "Build WiX MSI Installer for Lemonade Server"
    Write-Host ""
    Write-Host "Usage: .\build_installer_wix.ps1 [-Configuration <Debug|Release>] [-Help]"
    Write-Host ""
    Write-Host "Options:"
    Write-Host "  -Configuration   Build configuration (Debug or Release). Default: Release"
    Write-Host "  -Help            Show this help message"
    Write-Host ""
    Write-Host "Prerequisites:"
    Write-Host "  - WiX Toolset 5.0.2 installed"
    Write-Host "  - Visual Studio 2019 or later"
    Write-Host "  - CMake 3.20 or higher"
    exit 0
}

# Script configuration
$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
# Navigate to repository root (CMakeLists.txt is at top level now)
$RepoRoot = Resolve-Path (Join-Path $ScriptDir "..\..")
$BuildDir = Join-Path $RepoRoot "build"

Write-Host "================================================" -ForegroundColor Cyan
Write-Host "  Lemonade Server - WiX MSI Installer Build" -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan
Write-Host ""

# Check for WiX Toolset (v5.0+)
Write-Host "Checking for WiX Toolset..." -ForegroundColor Yellow
$wix = Get-Command wix -ErrorAction SilentlyContinue
if ($null -eq $wix) {
    Write-Host "ERROR: WiX Toolset not found!" -ForegroundColor Red
    Write-Host ""
    Write-Host "Please install WiX Toolset 5.0.2 from:" -ForegroundColor Yellow
    Write-Host "  https://github.com/wixtoolset/wix/releases/download/v5.0.2/wix-cli-x64.msi" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "After installation, restart your terminal and try again." -ForegroundColor Yellow
    Write-Host ""
    exit 1
}

# Get WiX version
$wixVersion = & wix --version 2>&1 | Select-String -Pattern "version ([\d\.]+)" | ForEach-Object { $_.Matches.Groups[1].Value }
Write-Host "  Found: wix version $wixVersion" -ForegroundColor Green
Write-Host ""

# Check for CMake
Write-Host "Checking for CMake..." -ForegroundColor Yellow
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($null -eq $cmake) {
    Write-Host "ERROR: CMake not found in PATH!" -ForegroundColor Red
    Write-Host "Please install CMake 3.20 or higher." -ForegroundColor Yellow
    exit 1
}
Write-Host "  Found: $($cmake.Source)" -ForegroundColor Green
Write-Host ""

# Step 1: Configure with CMake (if build directory doesn't exist)
if (-not (Test-Path $BuildDir)) {
    Write-Host "Configuring project with CMake..." -ForegroundColor Yellow
    cmake -S $RepoRoot -B $BuildDir
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: CMake configuration failed!" -ForegroundColor Red
        exit $LASTEXITCODE
    }
    Write-Host "  Configuration complete" -ForegroundColor Green
    Write-Host ""
} else {
    Write-Host "Build directory exists, skipping configuration" -ForegroundColor Green
    Write-Host ""
}

# Step 2: Build the project
Write-Host "Building Lemonade Server ($Configuration)..." -ForegroundColor Yellow
cmake --build $BuildDir --config $Configuration
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Build failed!" -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host "  Build complete" -ForegroundColor Green
Write-Host ""

# Step 3: Build both MSI installers
Write-Host "Building WiX MSI installers..." -ForegroundColor Yellow
cmake --build $BuildDir --config $Configuration --target wix_installers
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: MSI build failed!" -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host ""

# Success!
$MinimalMsi = Join-Path $RepoRoot "lemonade-server-minimal.msi"
$FullMsi = Join-Path $RepoRoot "lemonade.msi"

Write-Host "================================================" -ForegroundColor Green
Write-Host "  SUCCESS!" -ForegroundColor Green
Write-Host "================================================" -ForegroundColor Green
Write-Host ""

if (Test-Path $MinimalMsi) {
    $size = (Get-Item $MinimalMsi).Length / 1MB
    Write-Host "Minimal server installer:" -ForegroundColor Cyan
    Write-Host "  Path: $MinimalMsi" -ForegroundColor White
    Write-Host "  Size: $([math]::Round($size, 2)) MB" -ForegroundColor White
    Write-Host "  Install: msiexec /i lemonade-server-minimal.msi" -ForegroundColor Yellow
    Write-Host "  Silent : msiexec /i lemonade-server-minimal.msi /qn" -ForegroundColor Yellow
    Write-Host ""
} else {
    Write-Host "WARNING: lemonade-server-minimal.msi was not found!" -ForegroundColor Yellow
}

if (Test-Path $FullMsi) {
    $size = (Get-Item $FullMsi).Length / 1MB
    Write-Host "Full installer (with Electron app):" -ForegroundColor Cyan
    Write-Host "  Path: $FullMsi" -ForegroundColor White
    Write-Host "  Size: $([math]::Round($size, 2)) MB" -ForegroundColor White
    Write-Host "  Install: msiexec /i lemonade.msi" -ForegroundColor Yellow
    Write-Host "  Silent : msiexec /i lemonade.msi /qn" -ForegroundColor Yellow
    Write-Host ""
} else {
    Write-Host "WARNING: lemonade.msi was not found (Python 3 + Electron build required)." -ForegroundColor Yellow
}
