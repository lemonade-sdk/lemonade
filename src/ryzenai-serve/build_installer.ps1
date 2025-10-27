# Build RyzenAI Server Installer
# This script assumes CMake has already built ryzenai-serve.exe in build\bin\Release

Write-Host "Building RyzenAI Server Installer..." -ForegroundColor Cyan

# Check if NSIS is installed
$nsisPath = "C:\Program Files (x86)\NSIS\makensis.exe"
if (-not (Test-Path $nsisPath)) {
    Write-Host "ERROR: NSIS not found at $nsisPath" -ForegroundColor Red
    Write-Host "Please install NSIS from https://nsis.sourceforge.io/Download" -ForegroundColor Yellow
    exit 1
}

# Check if ryzenai-serve.exe exists
$exePath = "build\bin\Release\ryzenai-serve.exe"
if (-not (Test-Path $exePath)) {
    Write-Host "ERROR: $exePath not found!" -ForegroundColor Red
    Write-Host "Please build the project first with CMake:" -ForegroundColor Yellow
    Write-Host "  cd build" -ForegroundColor Yellow
    Write-Host "  cmake .." -ForegroundColor Yellow
    Write-Host "  cmake --build . --config Release" -ForegroundColor Yellow
    exit 1
}

Write-Host "Found ryzenai-serve.exe" -ForegroundColor Green

# Build the installer
Write-Host "Running NSIS..." -ForegroundColor Cyan
& $nsisPath "RyzenAI_Server_Installer.nsi"

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nInstaller built successfully!" -ForegroundColor Green
    Write-Host "Output: RyzenAI_Server_Installer.exe" -ForegroundColor Green
} else {
    Write-Host "`nInstaller build failed!" -ForegroundColor Red
    exit 1
}

