# Lemonade Installation Folder Diagnostic Script
# This script helps diagnose why the installation folder might be "in use"

param(
    [Parameter(Mandatory=$false)]
    [string]$InstallPath = "$env:LOCALAPPDATA\lemonade_server"
)

Write-Host "=== Lemonade Installation Folder Diagnostic ===" -ForegroundColor Cyan
Write-Host "Checking folder: $InstallPath" -ForegroundColor Yellow
Write-Host ""

# Check if folder exists
if (-not (Test-Path $InstallPath)) {
    Write-Host "✓ Installation folder does not exist - no conflicts expected" -ForegroundColor Green
    exit 0
}

Write-Host "📁 Installation folder exists, running diagnostics..." -ForegroundColor Yellow
Write-Host ""

# 1. Check for running lemonade/llama processes
Write-Host "1. Checking for running processes..." -ForegroundColor Cyan
try {
    $processes = Get-Process | Where-Object {
        $_.ProcessName -like "*lemonade*" -or 
        $_.ProcessName -like "*llama*" -or 
        ($_.ProcessName -like "*python*" -and $_.Path -like "*lemonade*")
    }
    
    if ($processes) {
        Write-Host "⚠️  Found potentially interfering processes:" -ForegroundColor Red
        $processes | Format-Table ProcessName, Id, Path -AutoSize
    } else {
        Write-Host "✓ No lemonade/llama processes found running" -ForegroundColor Green
    }
} catch {
    Write-Host "❌ Process check failed: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host ""

# 2. Check for processes with modules loaded from the installation folder
Write-Host "2. Checking for processes with loaded modules from installation folder..." -ForegroundColor Cyan
try {
    $processesWithModules = Get-Process | Where-Object {
        try {
            $_.Modules.FileName -like "*lemonade*"
        } catch {
            $false
        }
    }
    
    if ($processesWithModules) {
        Write-Host "⚠️  Found processes with lemonade modules loaded:" -ForegroundColor Red
        $processesWithModules | Format-Table ProcessName, Id, Path -AutoSize
    } else {
        Write-Host "✓ No processes found with lemonade modules loaded" -ForegroundColor Green
    }
} catch {
    Write-Host "❌ Module check failed: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host ""

# 3. Check for locked files in the installation directory
Write-Host "3. Checking for locked files in installation directory..." -ForegroundColor Cyan
try {
    $lockedFiles = @()
    Get-ChildItem $InstallPath -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
        try {
            $fileStream = [System.IO.File]::OpenWrite($_.FullName)
            $fileStream.Close()
        } catch {
            $lockedFiles += [PSCustomObject]@{
                File = $_.Name
                FullPath = $_.FullName
                Error = $_.Exception.Message
            }
        }
    }
    
    if ($lockedFiles) {
        Write-Host "⚠️  Found locked files:" -ForegroundColor Red
        $lockedFiles | Format-Table File, Error -AutoSize
    } else {
        Write-Host "✓ No locked files detected" -ForegroundColor Green
    }
} catch {
    Write-Host "❌ File lock check failed: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host ""

# 4. Check current working directories
Write-Host "4. Checking for processes with working directory in installation folder..." -ForegroundColor Cyan
try {
    $currentLocation = Get-Location
    $processesInFolder = Get-Process | Where-Object {
        try {
            # This is a simplified check - actual CWD detection is complex
            $currentLocation.Path -like "*lemonade*"
        } catch {
            $false
        }
    }
    
    if ($processesInFolder) {
        Write-Host "⚠️  Current PowerShell session is in lemonade folder" -ForegroundColor Red
        Write-Host "Current location: $($currentLocation.Path)" -ForegroundColor Yellow
    } else {
        Write-Host "✓ No obvious working directory conflicts" -ForegroundColor Green
    }
} catch {
    Write-Host "❌ Working directory check failed: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host ""

# 5. Test folder rename capability
Write-Host "5. Testing folder rename capability..." -ForegroundColor Cyan
$testPath = "$InstallPath.diagnostic_test"
try {
    Rename-Item $InstallPath $testPath -ErrorAction Stop
    Write-Host "✓ Folder rename successful - folder is not in use" -ForegroundColor Green
    Rename-Item $testPath $InstallPath -ErrorAction Stop
    Write-Host "✓ Folder restored successfully" -ForegroundColor Green
} catch {
    Write-Host "❌ Folder rename failed - folder IS in use!" -ForegroundColor Red
    Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
    
    # Additional Windows-specific checks
    Write-Host ""
    Write-Host "6. Additional Windows diagnostics..." -ForegroundColor Cyan
    
    # Check for open handles using handle.exe if available
    $handleExe = Get-Command "handle.exe" -ErrorAction SilentlyContinue
    if ($handleExe) {
        Write-Host "Running handle.exe to find open handles..." -ForegroundColor Yellow
        try {
            & handle.exe $InstallPath 2>$null | Where-Object { $_ -notlike "*No matching handles found*" }
        } catch {
            Write-Host "handle.exe check failed" -ForegroundColor Red
        }
    } else {
        Write-Host "handle.exe not found in PATH (part of Windows Sysinternals)" -ForegroundColor Yellow
        Write-Host "Download from: https://docs.microsoft.com/en-us/sysinternals/downloads/handle" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "=== Diagnostic Complete ===" -ForegroundColor Cyan
Write-Host ""
Write-Host "Common solutions if folder is in use:" -ForegroundColor Yellow
Write-Host "1. Close any File Explorer windows showing the installation folder"
Write-Host "2. Close any command prompts or PowerShell windows with CWD in the folder"
Write-Host "3. End any python.exe, lemonade-server.exe, or llama-server.exe processes"
Write-Host "4. Check Task Manager for any processes loading files from the folder"
Write-Host "5. Temporarily disable real-time antivirus scanning"
Write-Host "6. Restart Windows Explorer: taskkill /f /im explorer.exe && start explorer.exe"
Write-Host "7. Restart the computer as a last resort"