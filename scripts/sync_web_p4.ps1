# scripts/sync_web_p4.ps1
#
# Makes the device's web tree equal to a local `data` folder **over the network**:
# it asks the device which files it holds that the folder does not, and removes
# them. No cable and no SPIFFS image - this is the command-line form of what the
# Version page does, and it uses the same two routes (`POST /api/web/file` per
# file, then `POST /api/web/sync`), documented in Docs/Rest.md.
#
# Usage:
#     .\scripts\sync_web_p4.ps1 -Device 192.168.1.201
#         Dry run: lists the files that are on the device and not in the folder.
#     .\scripts\sync_web_p4.ps1 -Device 192.168.1.201 -Delete
#         Removes them (the device answers with what it deleted and what it could
#         not delete; every failure also lands in /fat/logs/Errors.log).
#     .\scripts\sync_web_p4.ps1 -Device 192.168.1.201 -Upload -Delete
#         Sends the folder first (`POST /api/web/file` for each file), then prunes.
#         The prune is skipped when any file failed to arrive - deleting after a
#         partial upload would remove the files that did not make it.
#
# Params:
#     -Device    host, host:port or a full http(s):// URL (default 192.168.1.201).
#                With `https://` the script adds `-k`, because the device's own
#                certificate is self-signed.
#     -User      -Password   HTTP Basic credentials (default admin/admin).
#     -Folder    the folder to compare with (default <repo>\data). It has to hold
#                `index.html` - the same rule the page enforces - otherwise the
#                device would be compared against something that is not the web
#                interface, and the prune would remove pages that belong there.
#     -Upload    send the folder first.
#     -Delete    actually remove the extra files. Without it: a dry run.
#
# Exit code: 0 when the device answered and (with -Delete) removed everything;
# 1 on a refused request, a failed upload, or a file the device could not remove.

[CmdletBinding()]
param(
    [string]$Device = "192.168.1.201",
    [string]$User = "admin",
    [string]$Password = "admin",
    [string]$Folder = "",
    [switch]$Upload,
    [switch]$Delete
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Folder) { $Folder = Join-Path $RepoRoot "data" }

# Rule 39 in a script: the numbers this file reasons about, named once.
$kIndexFile = "index.html"        # the file that tells the web tree from a folder
$kMaxNameLen = 64                 # the limit the firmware enforces on a name
$kTempDir = Join-Path ([System.IO.Path]::GetTempPath()) "sync_web_p4"

# --- The device's URL ---------------------------------------------------------
$BaseUrl = $Device
if ($BaseUrl -notmatch '^https?://') { $BaseUrl = "http://$BaseUrl" }
$BaseUrl = $BaseUrl.TrimEnd('/')

$Curl = (Get-Command "curl.exe" -ErrorAction SilentlyContinue)
if (-not $Curl) {
    Write-Error "curl.exe not found. Windows 10/11 ships it; otherwise install curl and put it on PATH."
}
$Curl = $Curl.Source

# Basic auth is sent **preemptively** - as a header, not through `-u`. The device
# answers a failed check with `401` (and a challenge), so `curl -u` would repeat
# every request with the credentials: two attempts per call, against a device that
# counts failed logins and locks the account for a while (Security page: max
# attempts / lockout period). With the header there is exactly one attempt per
# call, and a wrong password is reported at once instead of being retried.
$AuthHeader = "Authorization: Basic " +
    [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("${User}:${Password}"))

if (-not (Test-Path -LiteralPath $Folder -PathType Container)) {
    Write-Error "folder not found: $Folder"
}
if (-not (Test-Path -LiteralPath (Join-Path $Folder $kIndexFile))) {
    Write-Error "$Folder has no $kIndexFile - it is not the web-interface folder. Nothing was sent."
}

# --- The folder's files, in the shape the device expects ----------------------
# Relative, forward slashes, one name per segment of letters/digits/._- - the very
# rule `WebPrune::isAcceptableName()` enforces on the device. A name the device
# would refuse makes it answer 400 for the whole list, so it is caught here, by
# name, instead of as "the device refused".
$names = New-Object System.Collections.Generic.List[string]
$badNames = New-Object System.Collections.Generic.List[string]
$folderLen = (Resolve-Path -LiteralPath $Folder).Path.TrimEnd('\').Length + 1
foreach ($file in Get-ChildItem -LiteralPath $Folder -Recurse -File -Force) {
    $rel = $file.FullName.Substring($folderLen).Replace('\', '/')
    $segments = $rel.Split('/')
    $ok = $rel.Length -le $kMaxNameLen
    foreach ($seg in $segments) {
        if (-not $seg -or $seg -eq '.' -or $seg -eq '..' -or $seg -notmatch '^[A-Za-z0-9._-]+$') {
            $ok = $false
        }
    }
    if ($ok) { $names.Add($rel) } else { $badNames.Add($rel) }
}
if ($names.Count -eq 0) {
    Write-Error "$Folder holds no usable web files. Nothing was sent."
}
if ($badNames.Count -gt 0) {
    Write-Host "These names are not web-file names and would make the device refuse the whole list:" -ForegroundColor Red
    foreach ($n in $badNames) { Write-Host "    $n" -ForegroundColor Red }
    Write-Error "Rename or remove them and run again. Nothing was sent."
}

Write-Host "== Web-tree sync ==" -ForegroundColor Cyan
Write-Host "  device : $BaseUrl  (redirects followed, certificate not verified)"
Write-Host "  user   : $User  (Basic auth sent preemptively; the password is not printed)"
Write-Host "  folder : $Folder"
Write-Host "  files  : $($names.Count)"

# --- One JSON call with curl --------------------------------------------------
# The body goes through a file (`--data-binary "@file"`): a JSON list of paths is
# not something to fight PowerShell quoting over, and it is exactly what the
# browser sends.
#
# Two flags are always there, and both have a reason:
#   * `-L` - the device redirects plain HTTP to HTTPS whenever its switch is on
#     and the certificate is usable (stage 167: `301` for GET/HEAD, `308` for
#     everything else). A `308` preserves the method *and the body*, so following
#     it re-sends the very same POST; without `-L` the answer is that redirect
#     page, which is what this script used to report as a refusal.
#   * `-k` - the pair in the device is its own, self-signed one (the browser warns
#     about it in exactly the same way), and the request goes to the operator's own
#     device on his own network. It is verification that is switched off here, not
#     encryption.
function Invoke-DeviceJson {
    param([string]$Path, [string]$Body)
    if (-not (Test-Path -LiteralPath $kTempDir)) {
        New-Item -ItemType Directory -Path $kTempDir | Out-Null
    }
    $bodyFile = Join-Path $kTempDir "body.json"
    $respFile = Join-Path $kTempDir "response.json"
    [System.IO.File]::WriteAllText($bodyFile, $Body, (New-Object System.Text.UTF8Encoding($false)))

    $curlArgs = @("-L", "-k", "-s", "-H", $AuthHeader, "-X", "POST",
              "-H", "Content-Type: application/json",
              "--data-binary", "@$bodyFile",
              "-o", $respFile, "-w", "%{http_code}")
    $code = & $Curl @curlArgs "$BaseUrl$Path"
    $text = if (Test-Path -LiteralPath $respFile) { Get-Content -LiteralPath $respFile -Raw } else { "" }
    $json = $null
    try { $json = $text | ConvertFrom-Json } catch { }
    return [pscustomobject]@{ Code = "$code".Trim(); Json = $json; Text = $text }
}

# --- Uploading the folder (optional) ------------------------------------------
if ($Upload) {
    Write-Host "== Uploading the folder (POST /api/web/file) ==" -ForegroundColor Cyan
    $sent = 0
    $failed = New-Object System.Collections.Generic.List[string]
    foreach ($rel in $names) {
        $file = Join-Path $Folder ($rel.Replace('/', '\'))
        $quoted = [uri]::EscapeDataString($rel)
        $curlArgs = @("-L", "-k", "-s", "-H", $AuthHeader, "-X", "POST",
                  "-H", "Content-Type: application/octet-stream",
                  "--data-binary", "@$file", "-o", "$kTempDir\upload.json", "-w", "%{http_code}")
        $code = & $Curl @curlArgs "$BaseUrl/api/web/file?path=$quoted"
        if ("$code".Trim() -eq "401" -or "$code".Trim() -eq "403") {
            # A credential problem is not a per-file problem: stop before the
            # device counts another failed login (it locks the account after a
            # few - Security page: max attempts / lockout period).
            Write-Error "The device refused the credentials (HTTP $("$code".Trim())) on the first file - nothing else was sent. Pass the username and password you use on the Login page: -User ... -Password ... . If they are right, the device may have locked the account after several failed attempts; wait a few minutes and run again."
        }
        if ("$code".Trim() -eq "200") {
            $sent++
            Write-Host ("    {0,4}/{1}  {2}" -f $sent, $names.Count, $rel)
        } else {
            $failed.Add($rel)
            $detail = ""
            if (Test-Path -LiteralPath "$kTempDir\upload.json") {
                $detail = (Get-Content -LiteralPath "$kTempDir\upload.json" -Raw).Trim()
            }
            Write-Host ("    FAILED  {0}  (HTTP {1} {2})" -f $rel, "$code".Trim(), $detail) -ForegroundColor Red
        }
    }
    if ($failed.Count -gt 0) {
        Write-Host "$($failed.Count) file(s) did not arrive:" -ForegroundColor Red
        foreach ($f in $failed) { Write-Host "    $f" -ForegroundColor Red }
        Write-Error "The tree was NOT pruned: deleting now would remove the files that did not arrive."
    }
    Write-Host "  uploaded: $sent file(s)" -ForegroundColor Green
}

# --- What the folder does not hold --------------------------------------------
$paths = @($names)
$body = @{ paths = $paths } | ConvertTo-Json -Compress
$check = Invoke-DeviceJson -Path "/api/web/sync" -Body $body

if ($check.Code -eq "401" -or $check.Code -eq "403") {
    Write-Error "The device refused the credentials (HTTP $($check.Code)): $($check.Text). Pass the username and password you use on the Login page: -User ... -Password ... . If they are right, the device may have locked the account after several failed attempts (Security page: max attempts / lockout period); wait a few minutes and run again."
}
if ($check.Code -eq "404" -or $check.Code -eq "405") {
    Write-Error "The device does not answer /api/web/sync (HTTP $($check.Code)) - it runs firmware without it. Flash the build of stage 169 or later, then run this again."
}
if ($check.Code -ne "200" -or -not $check.Json) {
    Write-Error "The device refused the request (HTTP $($check.Code)): $($check.Text)"
}

$extra = @($check.Json.extra)
if ($extra.Count -eq 0) {
    Write-Host "Nothing to remove: the device holds the same files as the folder." -ForegroundColor Green
    exit 0
}

Write-Host "On the device and not in the folder ($($extra.Count)):" -ForegroundColor Yellow
foreach ($name in $extra) { Write-Host "    $name" }

if (-not $Delete) {
    Write-Host "Dry run: nothing was removed. Add -Delete to remove these files." -ForegroundColor Yellow
    exit 0
}

# --- Removing them ------------------------------------------------------------
$body = @{ paths = $paths; delete = $true } | ConvertTo-Json -Compress
$done = Invoke-DeviceJson -Path "/api/web/sync" -Body $body
if ($done.Code -eq "401" -or $done.Code -eq "403") {
    Write-Error "The device refused the credentials while removing (HTTP $($done.Code)): $($done.Text). Pass the username and password you use on the Login page: -User ... -Password ... ."
}
if ($done.Code -ne "200" -or -not $done.Json) {
    Write-Error "The device refused the removal (HTTP $($done.Code)): $($done.Text)"
}

$deleted = @($done.Json.deleted)
$notDeleted = @($done.Json.failed)
Write-Host "Removed: $($deleted.Count)" -ForegroundColor Green
foreach ($name in $deleted) { Write-Host "    - $name" }
if ($notDeleted.Count -gt 0) {
    Write-Host "Could not be removed: $($notDeleted.Count) (the device wrote each one into Errors.log):" -ForegroundColor Red
    foreach ($name in $notDeleted) { Write-Host "    ! $name" -ForegroundColor Red }
    exit 1
}
Write-Host "The device's web tree is now the folder's tree." -ForegroundColor Green
exit 0
