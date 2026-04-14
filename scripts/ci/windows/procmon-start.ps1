<#
.SYNOPSIS
    Downloads Sysinternals Process Monitor and starts a background capture session.

.DESCRIPTION
    Downloads ProcMon from Sysinternals, accepts the EULA silently via registry,
    and starts a non-interactive background capture that writes to a PML backing
    file. All process activity (including DLL load attempts by sd-cli.exe and
    lemonade-server.exe) is recorded.

    Call procmon-stop-and-report.ps1 afterwards to terminate the capture, convert
    the PML to CSV, and produce the missing-DLL filtered report.

.PARAMETER BackingFile
    Path to the PML capture file.
    Defaults to "$env:RUNNER_TEMP\procmon-capture.pml".

.PARAMETER ProcMonDir
    Directory into which Procmon.exe is downloaded/extracted.
    Defaults to "$env:RUNNER_TEMP\procmon".
#>
param(
    [string]$BackingFile = "$env:RUNNER_TEMP\procmon-capture.pml",
    [string]$ProcMonDir  = "$env:RUNNER_TEMP\procmon"
)

$ErrorActionPreference = "Stop"

Write-Host "=== ProcMon Diagnostic: START ===" -ForegroundColor Cyan

# Download and extract ProcMon if not already present
$procmonExe = Join-Path $ProcMonDir "Procmon.exe"
if (-not (Test-Path $procmonExe)) {
    $procmonZip = Join-Path $env:RUNNER_TEMP "ProcessMonitor.zip"
    Write-Host "Downloading Sysinternals Process Monitor..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Force -Path $ProcMonDir | Out-Null
    Invoke-WebRequest -Uri "https://download.sysinternals.com/files/ProcessMonitor.zip" `
                      -OutFile $procmonZip -UseBasicParsing
    Expand-Archive -Path $procmonZip -DestinationPath $ProcMonDir -Force
    Write-Host "ProcMon extracted to: $ProcMonDir" -ForegroundColor Green
} else {
    Write-Host "ProcMon already available at: $procmonExe" -ForegroundColor Green
}

# Accept EULA via registry so ProcMon does not prompt
Write-Host "Accepting ProcMon EULA via registry..." -ForegroundColor Yellow
reg add "HKCU\Software\Sysinternals\Process Monitor" /v "EulaAccepted" /t REG_DWORD /d 1 /f | Out-Null

# Persist paths to GITHUB_ENV so subsequent steps can locate them
"PROCMON_EXE=$procmonExe"          | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
"PROCMON_BACKING_FILE=$BackingFile" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append

# Start ProcMon in background; /Quiet suppresses the UI, /Minimized keeps it
# off-screen, /BackingFile sets where the PML is written.
Write-Host "Starting ProcMon capture -> $BackingFile" -ForegroundColor Cyan
Start-Process -FilePath $procmonExe `
              -ArgumentList "/AcceptEula", "/Quiet", "/Minimized", "/BackingFile", "`"$BackingFile`"" `
              -WindowStyle Minimized

# Allow ProcMon a moment to initialise before the test begins
Start-Sleep -Seconds 3

Write-Host "ProcMon capture running in background." -ForegroundColor Green
