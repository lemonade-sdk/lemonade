# ProcMon-based diagnostic script for missing DLL failures on Windows.
# Downloads Sysinternals Process Monitor, captures a short trace while running
# sd-cli.exe, then filters the exported CSV for NAME NOT FOUND / PATH NOT FOUND
# entries on .dll files so CI logs show exactly which DLLs are missing.
#
# Usage: .\ci\procmon-dll-diagnose.ps1
# Expected env vars: RUNNER_TEMP, LEMONADE_CACHE_DIR (or run from repo root)

$ErrorActionPreference = "Continue"

# Delay constants (seconds) – tuned for CI runner startup/shutdown latency
$PROCMON_START_WAIT  = 3   # time for ProcMon to initialize its kernel driver
$POST_RUN_WAIT       = 2   # drain remaining events after sd-cli exits
$PROCMON_STOP_WAIT   = 3   # time for ProcMon to flush and close the backing file
$CSV_EXPORT_WAIT     = 5   # time for ProcMon to convert .pml to .csv

$sdCliRelPath = ".\ci-cache\bin\sd-cpp\rocm-preview\sd-cli.exe"

if (-not (Test-Path $sdCliRelPath)) {
    Write-Host "sd-cli.exe not found at $sdCliRelPath - skipping ProcMon diagnostics"
    exit 0
}

$sdCli = (Resolve-Path $sdCliRelPath).Path
Write-Host "sd-cli.exe: $sdCli"

# Download and extract ProcMon
$procmonZip = Join-Path $env:RUNNER_TEMP "Procmon.zip"
$procmonDir = Join-Path $env:RUNNER_TEMP "Procmon"

New-Item -ItemType Directory -Force -Path $procmonDir | Out-Null

Write-Host "Downloading Process Monitor..."
Invoke-WebRequest -Uri "https://download.sysinternals.com/files/ProcessMonitor.zip" `
    -OutFile $procmonZip -UseBasicParsing

Expand-Archive -Path $procmonZip -DestinationPath $procmonDir -Force

$procmon = Join-Path $procmonDir "Procmon64.exe"
$pml     = Join-Path $env:RUNNER_TEMP "sd-cli.pml"
$csv     = Join-Path $env:RUNNER_TEMP "sd-cli.csv"

# Start ProcMon capture in the background; capture the process so we can verify it launched
Write-Host "Starting ProcMon capture..."
$pmProc = Start-Process -FilePath $procmon `
    -ArgumentList "/AcceptEula", "/Quiet", "/Minimized", "/BackingFile", $pml `
    -PassThru
if (-not $pmProc -or $pmProc.HasExited) {
    Write-Host "WARNING: ProcMon failed to start – diagnostics will be unavailable"
}
Start-Sleep -Seconds $PROCMON_START_WAIT

# Run sd-cli.exe once to reproduce the loader failure and capture its DLL look-ups
Write-Host "Running sd-cli.exe to capture DLL load activity..."
& $sdCli -M upscale --help
$sdExitCode = $LASTEXITCODE
Write-Host "sd-cli.exe exit code: $sdExitCode"

Start-Sleep -Seconds $POST_RUN_WAIT

# Stop ProcMon (saves .pml automatically)
Write-Host "Stopping ProcMon..."
& $procmon /Quiet /Terminate
Start-Sleep -Seconds $PROCMON_STOP_WAIT

# Export the capture to CSV and verify the file was created
Write-Host "Exporting ProcMon log to CSV..."
& $procmon /OpenLog $pml /SaveAs $csv /AcceptEula
Start-Sleep -Seconds $CSV_EXPORT_WAIT

if (-not (Test-Path $csv)) {
    Write-Host "WARNING: CSV export not found at $csv – ProcMon export may have failed"
    exit $sdExitCode
}

# Filter for missing DLL entries that sd-cli.exe triggered
Write-Host ""
Write-Host "=== Likely missing DLLs (NAME NOT FOUND / PATH NOT FOUND for .dll files) ==="
$hits = Get-Content $csv |
    Where-Object { $_ -match "(?i)sd-cli\.exe" } |
    Where-Object { $_ -match "(?i)\.dll" } |
    Where-Object { $_ -match "NAME NOT FOUND|PATH NOT FOUND" }

if ($hits) {
    $hits | Select-Object -First 200 | ForEach-Object { Write-Host $_ }
} else {
    Write-Host "(no NAME NOT FOUND / PATH NOT FOUND .dll entries found for sd-cli.exe)"
}

# Exit with sd-cli.exe's exit code so the CI step fails when sd-cli.exe is broken
exit $sdExitCode
