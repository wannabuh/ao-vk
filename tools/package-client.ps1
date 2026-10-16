# Packages ao-vk into a distributable folder/ZIP without modifying the game client.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\package-client.ps1
#   powershell -ExecutionPolicy Bypass -File tools\package-client.ps1 -NoBuild
#   powershell -ExecutionPolicy Bypass -File tools\package-client.ps1 -OutputDir dist
#   powershell -ExecutionPolicy Bypass -File tools\package-client.ps1 -OriginalDll "C:\path\randy31_orig.dll"
#
# Output:
#   <OutputDir>\ao-vk-package\
#   <OutputDir>\ao-vk-package.zip
#
# The package includes randy31.dll, the AOPlugins plugin (AOPlugins\ao-vk.dll: the Renderer tab, aoplugin/) and an
# install.ps1 helper. The installer preserves the game's original randy31.dll as randy31_orig.dll once and puts
# ao-vk.dll into the client's AOPlugins folder; plugins in the client's ao-vk-plugins folder are left as they are.

param(
    [string]$OutputDir = "",
    [string]$Distro = "Ubuntu-26.04",
    [string]$OriginalDll = "",
    [switch]$NoBuild,
    [switch]$NoZip
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

function Fail([string]$message) {
    Write-Host "package-client: $message" -ForegroundColor Red
    exit 1
}

if (-not $OutputDir) { $OutputDir = Join-Path $repo "dist" }
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
$packageDir = Join-Path $OutputDir "ao-vk-package"
$dll = Join-Path $repo "build\linux-release\randy31.dll"
$plugin = Join-Path $repo "build\aoplugin\ao-vk.dll"

# 1. Build, unless requested not to.
if (-not $NoBuild) {
    $wslRepo = "/mnt/" + $repo.Substring(0, 1).ToLower() + ($repo.Substring(2) -replace '\\', '/')
    Write-Host "package-client: building in WSL ($Distro)"
    $buildScript = "cd '$wslRepo' && { [ -f build/linux-release/build.ninja ] || cmake --preset linux-release >/dev/null; } && cmake --build --preset linux-release" +
                   " && cd aoplugin && { [ -f ../build/aoplugin/build.ninja ] || cmake --preset linux-release >/dev/null; } && cmake --build --preset linux-release"
    & wsl -d $Distro -- bash -lc $buildScript
    if ($LASTEXITCODE -ne 0) { Fail "the build failed" }
}

if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
    Fail "no built DLL at $dll (build first or omit -NoBuild)"
}
if (-not (Test-Path -LiteralPath $plugin -PathType Leaf)) {
    Fail "no built plugin at $plugin (build first or omit -NoBuild)"
}

# 2. Recreate the package directory so removed source files don't linger.
if (Test-Path -LiteralPath $packageDir) {
    Remove-Item -LiteralPath $packageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $packageDir -Force | Out-Null

Copy-Item -LiteralPath $dll -Destination (Join-Path $packageDir "randy31.dll")
New-Item -ItemType Directory -Path (Join-Path $packageDir "AOPlugins") -Force | Out-Null
Copy-Item -LiteralPath $plugin -Destination (Join-Path $packageDir "AOPlugins\ao-vk.dll")

if ($OriginalDll) {
    if (-not (Test-Path -LiteralPath $OriginalDll -PathType Leaf)) {
        Fail "original DLL not found: $OriginalDll"
    }
    Copy-Item -LiteralPath $OriginalDll -Destination (Join-Path $packageDir "randy31_orig.dll")
}

# 3. Add a standalone installer. It does not build anything and does not alter files until run.
$installer = @'
param(
    [Parameter(Mandatory = $true)]
    [string]$Client
)
$ErrorActionPreference = "Stop"

function Fail([string]$message) {
    Write-Host "install: $message" -ForegroundColor Red
    exit 1
}

$package = $PSScriptRoot
$sourceDll = Join-Path $package "randy31.dll"
$sourcePlugin = Join-Path $package "AOPlugins\ao-vk.dll"
if (-not (Test-Path (Join-Path $Client "AnarchyOnline.exe"))) {
    Fail "no AnarchyOnline.exe in $Client"
}
if (Get-Process -Name "AnarchyOnline" -ErrorAction SilentlyContinue) {
    Fail "the game is running - close it first"
}
if (-not (Test-Path $sourceDll)) { Fail "package is missing randy31.dll" }
if (-not (Test-Path $sourcePlugin)) { Fail "package is missing AOPlugins\ao-vk.dll" }

$target = Join-Path $Client "randy31.dll"
$orig = Join-Path $Client "randy31_orig.dll"

# If an original DLL is included, install it as the backup only when no backup exists.
$packagedOriginal = Join-Path $package "randy31_orig.dll"
if (-not (Test-Path $orig)) {
    if (Test-Path $packagedOriginal) {
        Copy-Item $packagedOriginal $orig
    } elseif (Test-Path $target) {
        # Preserve the existing game DLL; do not overwrite an existing backup.
        Move-Item $target $orig
    } else {
        Fail "neither randy31.dll nor randy31_orig.dll exists in the client"
    }
}
Copy-Item $sourceDll $target -Force

$hash = (Get-FileHash $target -Algorithm SHA256).Hash.Substring(0, 12).ToLower()
Write-Host "install: randy31.dll installed to $Client ($hash)"

# The Renderer tab: a plugin for AOPlugins (its version.dll in the client folder loads the AOPlugins folder).
$plugins = Join-Path $Client "AOPlugins"
New-Item -ItemType Directory -Path $plugins -Force | Out-Null
Copy-Item $sourcePlugin $plugins -Force
Write-Host "install: ao-vk.dll installed to $plugins"
$version = Join-Path $Client "version.dll"
$isAOPlugins = (Test-Path $version) -and
    [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($version)).Contains("=== AOPlugins session")
if (-not $isAOPlugins) {
    Write-Host "install: note - AOPlugins isn't installed (version.dll), so there is no Renderer tab in the options window; settings are in randy-vk.ini" -ForegroundColor Yellow
}
Write-Host "install: done"
'@
Set-Content -LiteralPath (Join-Path $packageDir "install.ps1") -Value $installer -Encoding UTF8

# 4. Include a short readme and checksums for verifying the package.
$readme = @"
ao-vk package
=============

Contents:
- randy31.dll: ao-vk renderer
- AOPlugins\ao-vk.dll: the Renderer tab in the options window (F10), a plugin for AOPlugins
- install.ps1: installer helper

Install (PowerShell):
  powershell -ExecutionPolicy Bypass -File .\install.ps1 -Client "C:\path\to\client"

Close Anarchy Online before installing. The installer preserves the existing renderer as
randy31_orig.dll if a backup does not already exist, and puts ao-vk.dll into the client's
AOPlugins folder. The Renderer tab needs AOPlugins (its version.dll in the client folder);
without it, settings are changed in randy-vk.ini or with the hotkeys. Review install.ps1
before running it.
"@
Set-Content -LiteralPath (Join-Path $packageDir "README.txt") -Value $readme -Encoding UTF8

$hashLines = Get-ChildItem -LiteralPath $packageDir -File -Recurse |
    Where-Object { $_.Name -ne "SHA256SUMS.txt" } |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($packageDir.Length).TrimStart('\')
        "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $relative
    }
Set-Content -LiteralPath (Join-Path $packageDir "SHA256SUMS.txt") -Value $hashLines -Encoding UTF8

Write-Host "package-client: files staged in $packageDir"
if (-not $NoZip) {
    $zip = Join-Path $OutputDir "ao-vk-package.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path (Join-Path $packageDir "*") -DestinationPath $zip -CompressionLevel Optimal
    Write-Host "package-client: ZIP created at $zip"
}
Write-Host "package-client: done"
