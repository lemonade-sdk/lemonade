# PowerShell script to build the full Lemonade installer (server + Electron app)
# This script builds both the C++ server and the Electron app, then creates the full MSI installer.

param(
    [Parameter(Mandatory=$false)]
    [string]$Configuration = "Release",
    
    [Parameter(Mandatory=$false)]
    [switch]$SkipElectronBuild = $false
)

$ErrorActionPreference = "Stop"

# Get script directory
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ScriptDir "build"

Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host "  Lemonade Full Installer Build Script" -ForegroundColor Cyan
Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host ""

# Check for WiX Toolset
Write-Host "Checking for WiX Toolset..." -ForegroundColor Yellow
$wix = Get-Command wix -ErrorAction SilentlyContinue
if ($null -eq $wix) {
    Write-Host "ERROR: WiX Toolset not found in PATH!" -ForegroundColor Red
    Write-Host "Please install WiX Toolset 5.0.2 or higher." -ForegroundColor Yellow
    Write-Host "Download from: https://github.com/wixtoolset/wix/releases/download/v5.0.2/wix-cli-x64.msi" -ForegroundColor Yellow
    exit 1
}

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

# Check for Node.js (required for Electron app)
if (-not $SkipElectronBuild) {
    Write-Host "Checking for Node.js..." -ForegroundColor Yellow
    $node = Get-Command node -ErrorAction SilentlyContinue
    if ($null -eq $node) {
        Write-Host "ERROR: Node.js not found in PATH!" -ForegroundColor Red
        Write-Host "Please install Node.js 18 or higher, or use -SkipElectronBuild flag." -ForegroundColor Yellow
        exit 1
    }
    $nodeVersion = & node --version
    Write-Host "  Found: Node.js $nodeVersion" -ForegroundColor Green
    Write-Host ""
}

# Step 1: Build Electron App (if not skipped)
if (-not $SkipElectronBuild) {
    Write-Host "Building Electron App..." -ForegroundColor Yellow
    $ElectronAppDir = Join-Path $ScriptDir "..\app"
    
    if (-not (Test-Path $ElectronAppDir)) {
        Write-Host "ERROR: Electron app directory not found: $ElectronAppDir" -ForegroundColor Red
        exit 1
    }
    
    Push-Location $ElectronAppDir
    try {
        Write-Host "  Installing npm dependencies..." -ForegroundColor Gray
        npm install
        if ($LASTEXITCODE -ne 0) {
            Write-Host "ERROR: npm install failed!" -ForegroundColor Red
            exit $LASTEXITCODE
        }
        
        Write-Host "  Building Electron app for Windows..." -ForegroundColor Gray
        npm run build:win
        if ($LASTEXITCODE -ne 0) {
            Write-Host "ERROR: Electron app build failed!" -ForegroundColor Red
            exit $LASTEXITCODE
        }
        
        # Verify the build output
        $ElectronExe = Join-Path $ElectronAppDir "dist-app\win-unpacked\Lemonade.exe"
        if (-not (Test-Path $ElectronExe)) {
            Write-Host "ERROR: Electron app executable not found: $ElectronExe" -ForegroundColor Red
            exit 1
        }
        
        Write-Host "  Electron app build complete!" -ForegroundColor Green
    }
    finally {
        Pop-Location
    }
    Write-Host ""
} else {
    Write-Host "Skipping Electron app build (using existing build)..." -ForegroundColor Yellow
    Write-Host ""
}

# Step 2: Configure with CMake (if build directory doesn't exist)
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

# Step 3: Build the C++ server
Write-Host "Building Lemonade Server ($Configuration)..." -ForegroundColor Yellow
cmake --build $BuildDir --config $Configuration
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Build failed!" -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host "  Build complete" -ForegroundColor Green
Write-Host ""

# Step 4: Build the full MSI installer
Write-Host "Building Full WiX MSI installer (server + Electron app)..." -ForegroundColor Yellow
cmake --build $BuildDir --config $Configuration --target wix_installer_full
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: MSI build failed!" -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host ""

# Success!
$MsiPath = Join-Path $ScriptDir "lemonade.msi"
if (Test-Path $MsiPath) {
    $MsiSize = (Get-Item $MsiPath).Length / 1MB
    Write-Host "==============================================================" -ForegroundColor Green
    Write-Host "  SUCCESS!" -ForegroundColor Green
    Write-Host "==============================================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "Full MSI installer created:" -ForegroundColor Green
    Write-Host "  Location: $MsiPath" -ForegroundColor White
    Write-Host "  Size: $([math]::Round($MsiSize, 2)) MB" -ForegroundColor White
    Write-Host ""
    Write-Host "To install:" -ForegroundColor Yellow
    Write-Host "  msiexec /i lemonade.msi" -ForegroundColor White
    Write-Host ""
    Write-Host "Or double-click the MSI file for GUI installation." -ForegroundColor Gray
    Write-Host ""
} else {
    Write-Host "ERROR: MSI file not found at expected location: $MsiPath" -ForegroundColor Red
    exit 1
}

