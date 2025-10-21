# Test lemon.cpp with existing Hugging Face cache

Write-Host "=========================================" -ForegroundColor Cyan
Write-Host "Testing lemon.cpp with Existing HF Cache" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan
Write-Host ""

# Check if lemonade binary exists
if (-not (Test-Path ".\lemonade.exe")) {
    Write-Host "Error: lemonade.exe not found" -ForegroundColor Red
    Write-Host "Please build first in build\Release\" -ForegroundColor Yellow
    exit 1
}

Write-Host "1. Checking HF cache location..."
$HFCache = "$env:USERPROFILE\.cache\huggingface\hub"
if (Test-Path $HFCache) {
    Write-Host "✓ HF cache found at: $HFCache" -ForegroundColor Green
    $ModelDirs = Get-ChildItem -Path $HFCache -Directory -Filter "models--*"
    $ModelCount = $ModelDirs.Count
    Write-Host "  Found $ModelCount model directories"
    Write-Host ""
    Write-Host "  Sample models in cache:"
    $ModelDirs | Select-Object -First 5 | ForEach-Object { Write-Host "    $($_.Name)" }
} else {
    Write-Host "✗ HF cache not found at: $HFCache" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "2. Testing lemonade list command..."
Write-Host "   This should detect your existing downloaded models"
Write-Host ""
& .\lemonade.exe list

Write-Host ""
Write-Host "3. Check if lemonade detected any downloaded models..."
Write-Host "   Look for 'Yes' in the Downloaded column above"
Write-Host ""

Write-Host "4. Testing model info lookup..."
Write-Host "   Attempting to get info on first model in cache..."
if ($ModelDirs.Count -gt 0) {
    $FirstModel = $ModelDirs[0]
    Write-Host "   Cache dir: $($FirstModel.Name)"
    Write-Host "   Files in model:"
    Get-ChildItem -Path $FirstModel.FullName -File -Recurse | 
        Select-Object -First 5 | 
        ForEach-Object { Write-Host "     $($_.FullName)" }
}

Write-Host ""
Write-Host "=========================================" -ForegroundColor Cyan
Write-Host "Test Complete" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "VERIFICATION CHECKLIST:"
Write-Host "[ ] Does 'lemonade list' run without errors?"
Write-Host "[ ] Does it show models you've already downloaded?"
Write-Host "[ ] Are the models marked with 'Yes' in Downloaded column?"
Write-Host ""
Write-Host "If the answers are YES, the cache compatibility is working!"

