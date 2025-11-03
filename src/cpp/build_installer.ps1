# Build Lemonade Server Beta Installer
# This script assumes CMake has already built lemonade-router.exe and lemonade-server-beta.exe in build\Release

Write-Host "Building Lemonade Server Beta Installer..." -ForegroundColor Cyan

# Check if NSIS is installed
$nsisPath = "C:\Program Files (x86)\NSIS\makensis.exe"
if (-not (Test-Path $nsisPath)) {
    Write-Host "ERROR: NSIS not found at $nsisPath" -ForegroundColor Red
    Write-Host "Please install NSIS from https://nsis.sourceforge.io/Download" -ForegroundColor Yellow
    exit 1
}

# Check if executables exist
$trayExe = "build\Release\lemonade-server-beta.exe"
$serverExe = "build\Release\lemonade-router.exe"

if (-not (Test-Path $trayExe)) {
    Write-Host "ERROR: $trayExe not found!" -ForegroundColor Red
    Write-Host "Please build the project first with CMake:" -ForegroundColor Yellow
    Write-Host "  cd build" -ForegroundColor Yellow
    Write-Host "  cmake .. -G `"Visual Studio 17 2022`"" -ForegroundColor Yellow
    Write-Host "  cmake --build . --config Release" -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path $serverExe)) {
    Write-Host "ERROR: $serverExe not found!" -ForegroundColor Red
    Write-Host "Please build the server executable first." -ForegroundColor Yellow
    exit 1
}

Write-Host "Found lemonade-server-beta.exe" -ForegroundColor Green
Write-Host "Found lemonade-router.exe" -ForegroundColor Green

# Check if resources directory exists
$resourcesDir = "build\Release\resources"
if (-not (Test-Path $resourcesDir)) {
    Write-Host "WARNING: Resources directory not found at $resourcesDir" -ForegroundColor Yellow
    Write-Host "The installer may not have icons. This is expected if CMake hasn't copied resources yet." -ForegroundColor Yellow
}

# Build the installer
Write-Host "Running NSIS..." -ForegroundColor Cyan
& $nsisPath "Lemonade_Server_Installer_beta.nsi"

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nInstaller built successfully!" -ForegroundColor Green
    Write-Host "Output: Lemonade_Server_Installer_beta.exe" -ForegroundColor Green
    Write-Host "`nYou can now run the installer to install Lemonade Server Beta to:" -ForegroundColor Cyan
    Write-Host "  $env:LOCALAPPDATA\lemonade_server_beta" -ForegroundColor Cyan
} else {
    Write-Host "`nInstaller build failed!" -ForegroundColor Red
    exit 1
}

