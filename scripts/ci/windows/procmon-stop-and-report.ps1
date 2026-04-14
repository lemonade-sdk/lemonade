<#
.SYNOPSIS
    Stops the ProcMon capture, converts the PML to CSV, and produces a
    missing-DLL / missing-library report.

.DESCRIPTION
    Terminates the running ProcMon process (which flushes the PML backing file),
    converts the PML to CSV using ProcMon's built-in /SaveAs switch, then filters
    the CSV for "NAME NOT FOUND" entries on DLL/EXE/SYS paths so that any missing
    runtime libraries attempted by sd-cli.exe or lemonade-server.exe are surfaced.

    The script reads PROCMON_EXE and PROCMON_BACKING_FILE from the environment
    (written by procmon-start.ps1) unless overridden by the parameters below.

    The output directory is written to PROCMON_REPORT_DIR in GITHUB_ENV so that
    the upload-artifact step can reference it.

.PARAMETER OutputDir
    Directory where the CSV and filtered report will be written.
    Defaults to "$env:RUNNER_TEMP\procmon-report".

.PARAMETER ProcMonExe
    Path to Procmon.exe. Defaults to $env:PROCMON_EXE.

.PARAMETER BackingFile
    Path to the PML backing file. Defaults to $env:PROCMON_BACKING_FILE.

.NOTES
    Interpreting the artifacts
    --------------------------
    procmon-capture.csv   - Full Process Monitor capture in CSV format.
                            Open in Excel / a text editor to inspect all events.
    missing-dlls-report.txt
                          - Filtered list of "NAME NOT FOUND" results for
                            .dll / .exe / .sys paths.  Any entry here is a
                            library that a process tried to load but could not
                            find on disk.  These are primary candidates for a
                            missing runtime or redistributable.
                            Columns: ProcessName | PID | Operation | Path
#>
param(
    [string]$OutputDir   = "$env:RUNNER_TEMP\procmon-report",
    [string]$ProcMonExe  = $env:PROCMON_EXE,
    [string]$BackingFile = $env:PROCMON_BACKING_FILE
)

$ErrorActionPreference = "Stop"

Write-Host "=== ProcMon Diagnostic: STOP & REPORT ===" -ForegroundColor Cyan

# Validate that ProcMon is available
if (-not $ProcMonExe -or -not (Test-Path $ProcMonExe)) {
    Write-Warning "Procmon.exe not found at '$ProcMonExe'. Skipping report."
    exit 0
}

if (-not $BackingFile) {
    Write-Warning "PROCMON_BACKING_FILE is not set. Skipping report."
    exit 0
}

# Terminate ProcMon, which also flushes the backing PML to disk
Write-Host "Terminating ProcMon capture (flushing PML)..." -ForegroundColor Yellow
& $ProcMonExe /Terminate
# Allow time for the flush to complete before reading the file
Start-Sleep -Seconds 5

if (-not (Test-Path $BackingFile)) {
    Write-Warning "PML file not found at '$BackingFile'. No capture data available."
    exit 0
}

# Create the output directory
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$csvFile    = Join-Path $OutputDir "procmon-capture.csv"
$reportFile = Join-Path $OutputDir "missing-dlls-report.txt"

# Convert PML -> CSV using ProcMon's built-in log conversion
Write-Host "Converting PML -> CSV: $csvFile" -ForegroundColor Yellow
& $ProcMonExe /AcceptEula /OpenLog "$BackingFile" /SaveAs "$csvFile"

# ProcMon /SaveAs is asynchronous; poll until the CSV appears (up to 120 s)
$timeout = 120
$elapsed = 0
while (-not (Test-Path $csvFile) -and $elapsed -lt $timeout) {
    Start-Sleep -Seconds 2
    $elapsed += 2
}

if (-not (Test-Path $csvFile)) {
    Write-Warning "CSV file was not produced within ${timeout}s. Skipping filtering."
    exit 0
}

$csvSizeKB = [math]::Round((Get-Item $csvFile).Length / 1KB, 1)
Write-Host "CSV produced: $csvFile (${csvSizeKB} KB)" -ForegroundColor Green

# -------------------------------------------------------------------
# Filter for NAME NOT FOUND on DLL/EXE/SYS paths
# ProcMon CSV columns (header row):
#   "Time of Day","Process Name","PID","Operation","Path","Result","Detail"
# -------------------------------------------------------------------
Write-Host "Filtering for missing libraries (NAME NOT FOUND)..." -ForegroundColor Yellow

$missingEntries = @()
try {
    Import-Csv $csvFile | Where-Object {
        $_.Result -eq "NAME NOT FOUND" -and
        ($_.Operation -match "Load Image|ReadFile|CreateFile") -and
        ($_.Path -match "\.(dll|exe|sys)$")
    } | ForEach-Object {
        $missingEntries += [PSCustomObject]@{
            ProcessName = $_."Process Name"
            PID         = $_.PID
            Operation   = $_.Operation
            Path        = $_.Path
            Result      = $_.Result
        }
    }
} catch {
    Write-Warning "Failed to parse CSV: $_"
}

# -------------------------------------------------------------------
# Write human-readable report
# -------------------------------------------------------------------
$reportLines = @()
$reportLines += "ProcMon Missing-DLL Diagnostic Report"
$reportLines += "Generated  : $((Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss')) UTC"
$reportLines += "PML source : $BackingFile"
$reportLines += "CSV source : $csvFile"
$reportLines += "=" * 70
$reportLines += ""

if ($missingEntries.Count -eq 0) {
    $reportLines += "No NAME NOT FOUND entries found for DLL/EXE/SYS paths."
    $reportLines += ""
    $reportLines += "This may mean:"
    $reportLines += "  - The capture window did not include the crash, or"
    $reportLines += "  - The crash has a different root cause (e.g. bad import table)."
} else {
    $reportLines += "Found $($missingEntries.Count) missing-library event(s)."
    $reportLines += ""
    $reportLines += "--- Summary (grouped by missing path) ---"
    $reportLines += ""

    $grouped = $missingEntries | Group-Object Path | Sort-Object Count -Descending
    foreach ($g in $grouped) {
        $procs = ($g.Group | Select-Object -ExpandProperty ProcessName -Unique) -join ", "
        $reportLines += "  MISSING : $($g.Name)"
        $reportLines += "  Tried by: $procs  ($($g.Count) occurrence(s))"
        $reportLines += ""
    }

    $reportLines += "--- Full event list ---"
    $reportLines += ""
    foreach ($entry in $missingEntries) {
        $reportLines += "  Process=$($entry.ProcessName) PID=$($entry.PID) " +
                        "Op=$($entry.Operation) Path=$($entry.Path)"
    }
}

$reportLines | Out-File -FilePath $reportFile -Encoding utf8
Write-Host "Missing-DLL report written to: $reportFile" -ForegroundColor Green

# Print the report inline so it is visible in the workflow log without
# needing to download the artifact
Write-Host ""
Write-Host ("=" * 66) -ForegroundColor Cyan
Write-Host " ProcMon Missing-DLL Summary" -ForegroundColor Cyan
Write-Host ("=" * 66) -ForegroundColor Cyan
Get-Content $reportFile | ForEach-Object { Write-Host $_ }
Write-Host ("=" * 66) -ForegroundColor Cyan

# Export the output directory path so the upload-artifact step can find it
"PROCMON_REPORT_DIR=$OutputDir" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append

exit 0
