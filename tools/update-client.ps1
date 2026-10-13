# Builds ao-vk (in WSL) and puts it into a client: randy31.dll into the client folder (the game's own kept as
# randy31_orig.dll). Plugins in the client's ao-vk-plugins folder (docs/ui-dll.md) are left as they are.
#
# Usage (from anywhere):  powershell -ExecutionPolicy Bypass -File tools\update-client.ps1 [-NoBuild] [-Client <dir>]
#                         [-Distro <WSL distro>]
param(
    [string]$Client = "C:\Users\$env:USERNAME\DownloadsPRK\client",
    [string]$Distro = "Ubuntu-26.04",
    [switch]$NoBuild
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

function Fail([string]$message) { Write-Host "update-client: $message" -ForegroundColor Red; exit 1 }

# The PE timestamp of a DLL (the original randy31.dll is 0x5CD328BA, the build ao-vk knows).
function PeTimestamp([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    return [BitConverter]::ToUInt32($bytes, $pe + 8)
}

if (-not (Test-Path (Join-Path $Client "AnarchyOnline.exe"))) { Fail "no AnarchyOnline.exe in $Client" }
if (Get-Process -Name "AnarchyOnline" -ErrorAction SilentlyContinue) { Fail "the game is running - close it first (its randy31.dll is in use)" }

# 1. Build.
$dll = Join-Path $repo "build\linux-release\randy31.dll"
if (-not $NoBuild) {
    $wslRepo = "/mnt/" + $repo.Substring(0, 1).ToLower() + ($repo.Substring(2) -replace '\\', '/')
    Write-Host "update-client: building in WSL ($Distro)"
    $script = "cd '$wslRepo' && { [ -f build/linux-release/build.ninja ] || cmake --preset linux-release >/dev/null; } && cmake --build --preset linux-release"
    wsl -d $Distro -- bash -lc $script
    if ($LASTEXITCODE -ne 0) { Fail "the build failed" }
}
if (-not (Test-Path $dll)) { Fail "no $dll - build first" }

# 2. The renderer: the game's randy31.dll becomes randy31_orig.dll (once), ours takes its place.
$target = Join-Path $Client "randy31.dll"
$orig = Join-Path $Client "randy31_orig.dll"
if (-not (Test-Path $orig)) {
    if (-not (Test-Path $target)) { Fail "neither randy31.dll nor randy31_orig.dll in $Client" }
    if ((PeTimestamp $target) -ne 0x5CD328BA) { Fail "$target is not the game's own randy31.dll and there is no randy31_orig.dll - put the original back first" }
    Rename-Item $target "randy31_orig.dll"
    Write-Host "update-client: the game's randy31.dll kept as randy31_orig.dll"
}
Copy-Item $dll $target -Force
$hash = (Get-FileHash $target -Algorithm SHA256).Hash.Substring(0, 12).ToLower()
Write-Host "update-client: randy31.dll -> $Client ($hash)"
Write-Host "update-client: done"
