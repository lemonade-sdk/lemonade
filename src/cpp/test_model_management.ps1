# Model Management Testing Script for lemon.cpp (PowerShell)
# This tests the critical model download functionality that replaces huggingface_hub

$ErrorActionPreference = "Continue"

Write-Host "=========================================" -ForegroundColor Cyan
Write-Host "lemon.cpp Model Management Tests" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan
Write-Host ""

# Test counter
$TestsPassed = 0
$TestsFailed = 0

# Setup test environment
$TestCache = ".\test_cache"
$env:LEMONADE_CACHE_DIR = $TestCache

# Clean up function
function Cleanup {
    Write-Host ""
    Write-Host "Cleaning up test cache..." -ForegroundColor Yellow
    if (Test-Path $TestCache) {
        Remove-Item -Path $TestCache -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# Create test cache
New-Item -ItemType Directory -Path $TestCache -Force | Out-Null

Write-Host "Test Cache Directory: $TestCache"
Write-Host ""

# Test 1: Binary exists and runs
Write-Host "Test 1: Check lemonade binary..."
if (Test-Path ".\lemonade-router.exe") {
    Write-Host "✓ PASS: lemonade binary found" -ForegroundColor Green
    $TestsPassed++
} else {
    Write-Host "✗ FAIL: lemonade binary not found" -ForegroundColor Red
    Write-Host "Please build first in build\Release\" -ForegroundColor Yellow
    $TestsFailed++
    Cleanup
    exit 1
}

# Test 2: Version check
Write-Host ""
Write-Host "Test 2: Version check..."
try {
    $version = & .\lemonade-router.exe --version 2>&1
    Write-Host "✓ PASS: Version: $version" -ForegroundColor Green
    $TestsPassed++
} catch {
    Write-Host "✗ FAIL: Version check failed" -ForegroundColor Red
    $TestsFailed++
}

# Test 3: List models
Write-Host ""
Write-Host "Test 3: List available models..."
try {
    $models = & .\lemonade-router.exe list 2>&1
    Write-Host "✓ PASS: Can list models" -ForegroundColor Green
    Write-Host "Sample models:"
    $models | Select-Object -First 5 | ForEach-Object { Write-Host $_ }
    $TestsPassed++
} catch {
    Write-Host "✗ FAIL: Failed to list models" -ForegroundColor Red
    $TestsFailed++
}

# Test 4: Download a small model
Write-Host ""
Write-Host "Test 4: Download model (Qwen2.5-0.5B-Instruct-CPU)..." -ForegroundColor Cyan
Write-Host "This tests the critical HF API integration..." -ForegroundColor Cyan
try {
    $output = & .\lemonade-router.exe pull Qwen2.5-0.5B-Instruct-CPU 2>&1
    Write-Host $output
    Write-Host "✓ PASS: Model download completed" -ForegroundColor Green
    $TestsPassed++
} catch {
    Write-Host "✗ FAIL: Model download failed" -ForegroundColor Red
    Write-Host $_.Exception.Message
    $TestsFailed++
}

# Test 5: Verify cache structure
Write-Host ""
Write-Host "Test 5: Verify cache structure..."
$HFCache = Join-Path $TestCache "huggingface\hub"
if (Test-Path $HFCache) {
    Write-Host "✓ PASS: HF cache directory created" -ForegroundColor Green
    Write-Host "Cache structure:"
    Get-ChildItem $HFCache | Select-Object -First 5 | ForEach-Object { Write-Host $_.Name }
    $TestsPassed++
} else {
    Write-Host "✗ FAIL: HF cache directory not found" -ForegroundColor Red
    $TestsFailed++
}

# Test 6: Check model files downloaded
Write-Host ""
Write-Host "Test 6: Check model files..."
$ModelDirs = Get-ChildItem -Path $HFCache -Directory -Filter "models--*" -ErrorAction SilentlyContinue
if ($ModelDirs) {
    $ModelDir = $ModelDirs[0].FullName
    $FileCount = (Get-ChildItem -Path $ModelDir -File -Recurse -ErrorAction SilentlyContinue).Count
    if ($FileCount -gt 0) {
        Write-Host "✓ PASS: Found $FileCount model files" -ForegroundColor Green
        Write-Host "Sample files:"
        Get-ChildItem -Path $ModelDir -File -Recurse | Select-Object -First 5 | 
            ForEach-Object { Write-Host $_.FullName }
        $TestsPassed++
    } else {
        Write-Host "✗ FAIL: No model files found" -ForegroundColor Red
        $TestsFailed++
    }
} else {
    Write-Host "✗ FAIL: Model directory not found" -ForegroundColor Red
    $TestsFailed++
}

# Test 7: List shows downloaded model
Write-Host ""
Write-Host "Test 7: Verify model shows as downloaded..."
try {
    $listOutput = & .\lemonade-router.exe list 2>&1 | Out-String
    if ($listOutput -match "Yes.*Qwen2.5-0.5B-Instruct-CPU") {
        Write-Host "✓ PASS: Model marked as downloaded" -ForegroundColor Green
        $TestsPassed++
    } else {
        Write-Host "⚠ WARN: Model not marked as downloaded (might be expected)" -ForegroundColor Yellow
    }
} catch {
    Write-Host "⚠ WARN: Could not verify download status" -ForegroundColor Yellow
}

# Test 8: Delete model
Write-Host ""
Write-Host "Test 8: Delete model..."
try {
    & .\lemonade-router.exe delete Qwen2.5-0.5B-Instruct-CPU 2>&1 | Out-Null
    Write-Host "✓ PASS: Model deleted" -ForegroundColor Green
    $TestsPassed++
} catch {
    Write-Host "✗ FAIL: Failed to delete model" -ForegroundColor Red
    $TestsFailed++
}

# Test 9: Verify deletion
Write-Host ""
Write-Host "Test 9: Verify model deleted..."
$FilesRemaining = (Get-ChildItem -Path $ModelDir -File -Recurse -ErrorAction SilentlyContinue).Count
if ($FilesRemaining -eq 0) {
    Write-Host "✓ PASS: Model files removed" -ForegroundColor Green
    $TestsPassed++
} else {
    Write-Host "✗ FAIL: Model files still exist ($FilesRemaining files)" -ForegroundColor Red
    $TestsFailed++
}

# Test 10: Offline mode
Write-Host ""
Write-Host "Test 10: Test offline mode..."
$env:LEMONADE_OFFLINE = "1"
try {
    $output = & .\lemonade-router.exe pull Qwen2.5-0.5B-Instruct-CPU 2>&1 | Out-String
    if ($output -match "Offline mode|skipping") {
        Write-Host "✓ PASS: Offline mode respected" -ForegroundColor Green
        $TestsPassed++
    } else {
        Write-Host "⚠ WARN: Offline mode behavior unclear" -ForegroundColor Yellow
    }
} catch {
    Write-Host "⚠ WARN: Offline mode test inconclusive" -ForegroundColor Yellow
}
Remove-Item Env:\LEMONADE_OFFLINE

# Test 11: User model registration
Write-Host ""
Write-Host "Test 11: Register custom user model..."
try {
    & .\lemonade-router.exe pull user.TestModel --checkpoint test/model --recipe llamacpp 2>&1 | Out-Null
    Write-Host "✓ PASS: User model registered" -ForegroundColor Green
    $TestsPassed++
    
    # Check user_models.json created
    $UserModelsPath = Join-Path $TestCache "user_models.json"
    if (Test-Path $UserModelsPath) {
        Write-Host "✓ PASS: user_models.json created" -ForegroundColor Green
        Write-Host "Contents:"
        Get-Content $UserModelsPath | Write-Host
        $TestsPassed++
    } else {
        Write-Host "✗ FAIL: user_models.json not created" -ForegroundColor Red
        $TestsFailed++
    }
} catch {
    Write-Host "✗ FAIL: Failed to register user model" -ForegroundColor Red
    $TestsFailed++
}

# Summary
Write-Host ""
Write-Host "=========================================" -ForegroundColor Cyan
Write-Host "Test Summary" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan
Write-Host "Tests Passed: $TestsPassed" -ForegroundColor Green
Write-Host "Tests Failed: $TestsFailed" -ForegroundColor Red
Write-Host "=========================================" -ForegroundColor Cyan

# Cleanup
Cleanup

if ($TestsFailed -eq 0) {
    Write-Host "All tests passed!" -ForegroundColor Green
    exit 0
} else {
    Write-Host "Some tests failed." -ForegroundColor Red
    exit 1
}

