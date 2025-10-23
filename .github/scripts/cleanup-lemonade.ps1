param(
    [string]$Context = "Cleanup",
    [bool]$FailOnError = $true,
    [bool]$CleanLogs = $false
)

Write-Host "=== $Context ==="
$cleanupErrors = @()

# Stop processes
$processPatterns = @("lemonade", "gaia", "python", "llama-server")
$foundProcesses = @()

foreach ($pattern in $processPatterns) {
    $processes = Get-Process | Where-Object { $_.ProcessName -like "*$pattern*" }
    if ($processes) {
        $foundProcesses += $processes
        Write-Host "Found processes matching '$pattern':"
        $processes | Format-Table ProcessName, Id, CPU -AutoSize
    }
}

if ($foundProcesses.Count -gt 0) {
    Write-Host "Stopping $($foundProcesses.Count) processes..."
    foreach ($process in $foundProcesses) {
        try {
            Write-Host "Stopping process: $($process.ProcessName) (PID: $($process.Id))"
            Stop-Process -Id $process.Id -Force -ErrorAction Stop
        } catch {
            $error = "Failed to stop process $($process.ProcessName) (PID: $($process.Id)): $_"
            Write-Host $error
            if ($FailOnError) {
                $cleanupErrors += $error
            }
        }
    }
    
    # Wait for processes to terminate
    Start-Sleep -Seconds 3
    
    # Verify processes are gone
    Write-Host "Verifying processes are terminated..."
    $remainingProcesses = @()
    foreach ($pattern in $processPatterns) {
        $processes = Get-Process | Where-Object { $_.ProcessName -like "*$pattern*" }
        if ($processes) {
            $remainingProcesses += $processes
        }
    }
    
    if ($remainingProcesses.Count -gt 0) {
        $warning = "$($remainingProcesses.Count) processes are still running after cleanup"
        Write-Host "Warning: $warning"
        $remainingProcesses | Format-Table ProcessName, Id, CPU -AutoSize
        if ($FailOnError) {
            $cleanupErrors += $warning
        }
    } else {
        Write-Host "Process cleanup verified - all target processes terminated"
    }
} else {
    Write-Host "No matching processes found"
}

# Clean up installation directory
$installPath = "C:\Users\nimbys\AppData\Local\lemonade_server"
if (Test-Path $installPath) {
    Write-Host "Removing installation directory: $installPath"
    try {
        Remove-Item -Path $installPath -Recurse -Force -ErrorAction Stop
        Write-Host "Successfully removed installation directory"
    } catch {
        $error = "Failed to remove installation directory: $_"
        Write-Host $error
        if ($FailOnError) {
            Write-Host "Contents that couldn't be removed:"
            Get-ChildItem $installPath -Recurse -ErrorAction SilentlyContinue | Format-Table FullName, Length -AutoSize
            $cleanupErrors += $error
        }
    }
} else {
    Write-Host "No installation directory found"
}

# Verify directory is gone (only if we expect it to be)
if ($FailOnError -and (Test-Path $installPath)) {
    $cleanupErrors += "Installation directory still exists after cleanup attempt"
}

# Clean up lemonade log files from temp directory (only if requested)
if ($CleanLogs) {
    Write-Host "Cleaning up lemonade log files from temp directory..."
    $tempPath = $env:TEMP
    $lemonadeLogFiles = Get-ChildItem "$tempPath\lemonade_*.log" -ErrorAction SilentlyContinue

    if ($lemonadeLogFiles) {
        Write-Host "Found $($lemonadeLogFiles.Count) lemonade log file(s) to remove:"
        $lemonadeLogFiles | Format-Table Name, LastWriteTime, Length -AutoSize
        
        foreach ($logFile in $lemonadeLogFiles) {
            try {
                Remove-Item -Path $logFile.FullName -Force -ErrorAction Stop
                Write-Host "Removed: $($logFile.Name)"
            } catch {
                $error = "Failed to remove log file $($logFile.Name): $_"
                Write-Host $error
                if ($FailOnError) {
                    $cleanupErrors += $error
                }
            }
        }
        
        # Verify log files are gone
        $remainingLogs = Get-ChildItem "$tempPath\lemonade_*.log" -ErrorAction SilentlyContinue
        if ($remainingLogs) {
            $warning = "$($remainingLogs.Count) lemonade log files still exist after cleanup"
            Write-Host "Warning: $warning"
            $remainingLogs | Format-Table Name, LastWriteTime -AutoSize
            if ($FailOnError) {
                $cleanupErrors += $warning
            }
        } else {
            Write-Host "Log cleanup verified - all lemonade log files removed"
        }
    } else {
        Write-Host "No lemonade log files found in temp directory"
    }
} else {
    Write-Host "Skipping log cleanup (CleanLogs = false)"
}

# Handle errors
if ($cleanupErrors.Count -gt 0) {
    Write-Host "$Context encountered errors:"
    foreach ($error in $cleanupErrors) {
        Write-Host "  $error"
    }
    if ($FailOnError) {
        exit 1
    }
} else {
    Write-Host "$Context completed successfully"
}
