# PowerShell script to harvest Electron renderer files for WiX installer
# This script generates WiX component definitions for all files in the renderer directory

param(
    [Parameter(Mandatory=$true)]
    [string]$RendererDir,
    
    [Parameter(Mandatory=$true)]
    [string]$OutputFile
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $RendererDir)) {
    Write-Error "Renderer directory not found: $RendererDir"
    exit 1
}

Write-Host "Harvesting renderer files from: $RendererDir" -ForegroundColor Cyan

# Get all files recursively
$files = Get-ChildItem -Path $RendererDir -Recurse -File

Write-Host "Found $($files.Count) files" -ForegroundColor Yellow

# Start writing XML
$xml = @"
<?xml version="1.0" encoding="UTF-8"?>
<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">
  <Fragment>
    <!-- Auto-generated renderer components -->
    <ComponentGroup Id="RendererComponents">

"@

# Generate component for each file
$counter = 0
foreach ($file in $files) {
    $relativePath = $file.FullName.Substring($RendererDir.Length + 1)
    $fileId = "Renderer_" + ($relativePath -replace '[\\/.:\-]', '_')
    $guid = [guid]::NewGuid().ToString()
    
    # Sanitize file ID to be XML-safe
    $fileId = $fileId -replace '[^a-zA-Z0-9_]', '_'
    
    $xml += @"
      <Component Id="$fileId" Directory="RendererDir" Guid="$guid">
        <File Id="$fileId" Source="`$(var.SourceDir)\build\Release\resources\dist\renderer\$relativePath" KeyPath="yes" />
      </Component>

"@
    
    $counter++
    if ($counter % 100 -eq 0) {
        Write-Host "  Processed $counter files..." -ForegroundColor Gray
    }
}

$xml += @"
    </ComponentGroup>
  </Fragment>
</Wix>
"@

# Write to output file
$xml | Out-File -FilePath $OutputFile -Encoding UTF8

Write-Host "Generated WiX components: $OutputFile" -ForegroundColor Green
Write-Host "  Total components: $counter" -ForegroundColor Green


