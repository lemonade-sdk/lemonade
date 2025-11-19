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
$BuildDir = Join-Path $ScriptDir "build"

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
    cmake -S $ScriptDir -B $BuildDir
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

# Step 3: Build the MSI installer
Write-Host "Building WiX MSI installer..." -ForegroundColor Yellow
cmake --build $BuildDir --config $Configuration --target wix_installer
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: MSI build failed!" -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host ""

# Success!
$MsiPath = Join-Path $ScriptDir "lemonade-server-minimal.msi"
if (Test-Path $MsiPath) {
    $MsiSize = (Get-Item $MsiPath).Length / 1MB
    Write-Host "================================================" -ForegroundColor Green
    Write-Host "  SUCCESS!" -ForegroundColor Green
    Write-Host "================================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "MSI installer created:" -ForegroundColor Cyan
    Write-Host "  Path: $MsiPath" -ForegroundColor White
    Write-Host "  Size: $([math]::Round($MsiSize, 2)) MB" -ForegroundColor White
    Write-Host ""
    Write-Host "To install:" -ForegroundColor Yellow
    Write-Host "  msiexec /i lemonade-server-minimal.msi" -ForegroundColor White
    Write-Host ""
    Write-Host "To install silently:" -ForegroundColor Yellow
    Write-Host "  msiexec /i lemonade-server-minimal.msi /qn" -ForegroundColor White
    Write-Host ""
} else {
    Write-Host "WARNING: MSI file not found at expected location!" -ForegroundColor Yellow
    Write-Host "  Expected: $MsiPath" -ForegroundColor Yellow
}


