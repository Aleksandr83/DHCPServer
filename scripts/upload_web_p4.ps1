# scripts/upload_web_p4.ps1
#
# Builds the web-UI SPIFFS image from the data/ directory and flashes it to the
# ESP32-P4 "spiffs" partition. This is the native ESP-IDF (idf.py) counterpart of
# the PlatformIO "uploadfs" target, which does not exist for the ESP32-P4.
#
# Usage (from an ESP-IDF terminal, i.e. with IDF_PATH exported):
#     .\scripts\upload_web_p4.ps1 -Port COM3
#
# Optional switches:
#     -Port       COM port of the ESP32-P4 (default: COM3)
#     -Baud       esptool baud rate (default: 460800)
#     -BuildOnly  build the SPIFFS image only, do not flash (default: false)
#     -ImageSize  override the SPIFFS image size in hex, e.g. 0x80000.
#                 Default is the full size of the "spiffs" partition read from
#                 partitions/dhcp_partitions_p4.csv, which is what ESP-IDF itself
#                 uses (spiffs_create_partition_image). Shrinking the image is
#                 NOT recommended: files already written beyond the new image
#                 would not be erased.
[CmdletBinding()]
param(
    [string]$Port = "COM3",
    [int]$Baud = 460800,
    [string]$ImageSize = "",
    [switch]$BuildOnly
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$DataDir  = Join-Path $RepoRoot "data"
$OutDir   = Join-Path $RepoRoot "build"
$OutImage = Join-Path $OutDir "web_ui_spiffs.bin"

function Get-FirstExisting {
    param([string[]]$Candidates)
    foreach ($c in $Candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { return $c }
    }
    return $null
}

# --- Locate the ESP-IDF Python environment and spiffsgen.py -----------------
$envCandidates = @()
if ($env:IDF_PYTHON_ENV_PATH) {
    $envCandidates += (Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe")
}
$Python = Get-FirstExisting @($envCandidates + @("C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe"))
if (-not $Python) {
    $cmd = Get-Command "python" -ErrorAction SilentlyContinue
    if ($cmd) { $Python = $cmd.Source } else { $Python = "python" }
}
$idfCandidates = @()
if ($env:IDF_PATH) {
    $idfCandidates += $env:IDF_PATH
}
$IdfPath = Get-FirstExisting @($idfCandidates + @("C:\esp\v6.0.1\esp-idf"))
if (-not $IdfPath) {
    Write-Error "ESP-IDF not found. Run this from an ESP-IDF terminal or set IDF_PATH."
}
$SpiffsGen = Join-Path $IdfPath "components\spiffs\spiffsgen.py"
if (-not (Test-Path -LiteralPath $SpiffsGen)) {
    Write-Error "spiffsgen.py not found: $SpiffsGen"
}

# --- Read the "spiffs" partition offset/size from the P4 partition table ----
$PartCsv = Join-Path $RepoRoot "partitions\dhcp_partitions_p4.csv"
$spiffsOffset = $null
$spiffsSize = $null
foreach ($line in Get-Content -LiteralPath $PartCsv) {
    $line = $line.Trim()
    if (-not $line -or $line.StartsWith("#")) { continue }
    $f = $line.Split(",") | ForEach-Object { $_.Trim() }
    if ($f[0] -eq "spiffs") {
        $spiffsOffset = $f[3]
        $spiffsSize   = $f[4]
        break
    }
}
if (-not $spiffsSize) {
    Write-Error "Could not find the 'spiffs' partition in $PartCsv"
}
if ($ImageSize) {
    $spiffsSize = $ImageSize
}
$Offset = [Convert]::ToUInt32($spiffsOffset, 16)
$Size   = [Convert]::ToUInt32($spiffsSize, 16)

# --- SPIFFS geometry: keep in sync with the SPIFFS Kconfig of the build -----
# Values below match sdkconfig/sdkconfig.defaults (page 256, block 4096,
# obj-name-len 32, meta-len 4, use-magic + use-magic-len).
if (-not (Test-Path -LiteralPath $DataDir)) {
    Write-Error "Web UI directory not found: $DataDir"
}
if (-not (Test-Path -LiteralPath $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}

Write-Host "== Building SPIFFS web-UI image ==" -ForegroundColor Cyan
Write-Host "  data dir : $DataDir"
Write-Host "  partition: spiffs @ 0x$($Offset.ToString('X')) size 0x$($Size.ToString('X'))"
$genArgs = @(
    "--page-size", "256",
    "--block-size", "4096",
    "--obj-name-len", "32",
    "--meta-len", "4",
    "--use-magic",
    "--use-magic-len",
    $spiffsSize,
    $DataDir,
    $OutImage
)
& $Python $SpiffsGen @genArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "  image    : $OutImage"

if ($BuildOnly) {
    Write-Host "Build-only: image ready, not flashing." -ForegroundColor Green
    exit 0
}

# --- Flash the image with esptool -------------------------------------------
Write-Host "== Flashing web UI to ESP32-P4 on $Port ==" -ForegroundColor Cyan
$esptoolArgs = @(
    "-m", "esptool",
    "--chip", "esp32p4",
    "-p", $Port,
    "-b", "$Baud",
    "--before=default-reset",
    "--after=hard-reset",
    "write_flash",
    "--flash-mode", "dio",
    "--flash-freq", "80m",
    "--flash-size", "32MB",
    "0x$($Offset.ToString('X'))", $OutImage
)
& $Python @esptoolArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "esptool failed with exit code $LASTEXITCODE"
}
Write-Host "Web UI upload complete." -ForegroundColor Green
