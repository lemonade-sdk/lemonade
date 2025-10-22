# PowerShell script to stop server and rebuild lemon.cpp

Write-Host "Checking for running server..." -ForegroundColor Cyan

# Try to stop the server if it's running
& ".\build\Release\lemonade.exe" stop 2>&1 | Out-Null

Write-Host "Building lemon.cpp..." -ForegroundColor Cyan

# Navigate to build directory and build
Push-Location build
cmake --build . --config Release
$buildResult = $LASTEXITCODE
Pop-Location

if ($buildResult -eq 0) {
    Write-Host "`nBuild successful!" -ForegroundColor Green
    Write-Host "`nYou can now run:" -ForegroundColor Yellow
    Write-Host "  .\build\Release\lemonade.exe serve" -ForegroundColor White
} else {
    Write-Host "`nBuild failed!" -ForegroundColor Red
    exit $buildResult
}

