# Runs ao-vk.dll in AOPlugins' fake client (AOPlugins/tests/fake_client) with ao-vk's real randy31.dll, and once
# without it, and checks: the renderer settings become DValues and a Renderer tab, presets and changes reach the
# renderer (randy-vk.ini), the frame rate cap (taken over from AOReloaded.ini, then changed in the window).
#
# Usage:  powershell -ExecutionPolicy Bypass -File aoplugin\test-fake-client.ps1 [-AOPlugins <its build dir>] [-Keep]
#         (needs ao-vk built - build\linux-release\randy31.dll - this plugin built - build\aoplugin\ao-vk.dll -
#         and AOPlugins built with its tests)
param(
    [string]$AOPlugins = "C:\projekter\AOPlugins\build\linux-release",
    [switch]$Keep
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$work = Join-Path $env:TEMP "ao-vk-aoplugin-test"
$failures = 0

function Check([string]$what, [bool]$ok) {
    if ($ok) { Write-Host "  ok    $what" -ForegroundColor Green }
    else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:failures++ }
}

function Run([string]$name, [bool]$withRenderer, [string]$sets) {
    Write-Host "== $name"
    $dir = Join-Path $work $name
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    $panel = Join-Path $dir "cd_image\gui\Default\OptionPanel"
    New-Item -ItemType Directory -Force (Join-Path $dir "AOPlugins"), $panel | Out-Null
    Copy-Item (Join-Path $AOPlugins "version.dll") $dir
    foreach ($f in "AnarchyOnline.exe", "Utils.dll", "AFCM.dll") { Copy-Item (Join-Path $AOPlugins "tests\$f") $dir }
    Copy-Item (Join-Path $repo "build\aoplugin\ao-vk.dll") (Join-Path $dir "AOPlugins")
    if ($withRenderer) { Copy-Item (Join-Path $repo "build\linux-release\randy31.dll") $dir }
    # A skin's static copy of the Renderer tab, and the AOReloaded fork's frame rate cap.
    Set-Content (Join-Path $panel "Root.xml") -NoNewline -Value (
        "<root>`n  <ScrollView label=`"General`"><View/></ScrollView>`n" +
        "  <ScrollView h_alignment=`"LEFT`" label=`"Renderer`"><TextView value=`"STALE`"/></ScrollView>`n</root>`n")
    Set-Content (Join-Path $dir "AOReloaded.ini") "[AOReloaded]`r`nAOR_FpsCap=144`r`nAOR_CharDist=150`r`nAOR_ObjDist=200`r`n"

    $env:FAKE_CLIENT_PRELOAD = if ($withRenderer) { "randy31.dll" } else { "" }
    $env:FAKE_CLIENT_SET = $sets
    $p = Start-Process -FilePath (Join-Path $dir "AnarchyOnline.exe") -WorkingDirectory $dir -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $dir "stdout.txt")
    $null = $p.Handle
    if (-not $p.WaitForExit(30000)) { $p.Kill(); Check "the fake client finished" $false; return $null }
    Remove-Item Env:\FAKE_CLIENT_PRELOAD, Env:\FAKE_CLIENT_SET -ErrorAction SilentlyContinue
    Check "the fake client exited cleanly (code $($p.ExitCode))" ($p.ExitCode -eq 0)

    $read = { param($f) $path = Join-Path $dir $f; if (Test-Path $path) { Get-Content -Raw $path } else { "" } }
    return [pscustomobject]@{
        Log = & $read "AOPlugins.log"; Ini = & $read "AOPlugins.ini"; Xml = & $read "served-Root.xml"
        Values = & $read "dvalues.txt"; Stdout = & $read "stdout.txt"; Randy = & $read "randy-vk.ini"
    }
}

function Has([string]$text, [string]$literal) { return $text.Contains($literal) }

$r = Run "renderer" $true "AOVK_FpsCap=double:200;RVK_Preset=double:3;RVK_PixelLight=bool:0;AOVK_ObjDist=double:250;DisplayGroundFullQualityRadius=double:30"
if ($r) {
    Check "ao-vk runs" ($r.Log -match "ao-vk [0-9.]+: running")
    Check "the renderer's settings are found" ($r.Log -match "\[ao-vk\] renderer settings: [1-9][0-9]* \(presets: yes\)")
    Check "they are DValues" (($r.Values -match "RVK_Enhance=bool:") -and ($r.Values -match "RVK_Preset=int:"))
    Check "the frame rate cap starts from AOReloaded.ini's" (Has $r.Log "[ao-vk] maximum frame rate 144")
    Check "... changes from the window" ((Has $r.Log "[ao-vk] maximum frame rate 200") -and (Has $r.Stdout "max frame rate = 200"))
    Check "... and is kept in AOPlugins.ini" ($r.Ini -match "(?s)\[ao-vk\][^\[]*AOVK_FpsCap=200")
    Check "a preset reaches the renderer" (Has $r.Log "[ao-vk] preset Ultra")
    Check "a change by hand reaches the renderer" (Has $r.Log "[ao-vk] RVK_PixelLight = 0")
    Check "... which saves it (randy-vk.ini)" ($r.Randy -match "RVK_PixelLight\s*=\s*0")
    Check "a change of a preset setting: the preset buttons show Custom" ($r.Values -match "RVK_Preset=int:99")
    Check "renderer settings aren't kept in AOPlugins.ini" (-not ($r.Ini -match "RVK_"))
    Check "one Renderer tab, the skin's copy replaced" ((([regex]::Matches($r.Xml, 'label="Renderer"')).Count -eq 1) -and -not (Has $r.Xml "STALE"))
    Check "view distance: AOReloaded.ini's 150 m goes back on the game thread after login (80: no N3 here to unclamp)" (Has $r.Log "[ao-vk] DisplayCharViewDistance 70 -> 80")
    Check "... the game's slider is remembered" ($r.Ini -match "AOVK_GroundHQ=30")
    Check "... object detail distance changes" (Has $r.Log "statel rings at 250%")
    Check "... without N3 / DisplaySystem nothing is patched, and the log says so" (Has $r.Log "view distance: characters stock (not patched), ground stock (not patched), objects stock (not patched)")
    Check "disconnect to login: without GUI.dll nothing is patched, and the log says so" (Has $r.Log "disconnect: lost connection stock (not patched), timeout stock (not patched)")
    Check "... the tab has the view distance note" (Has $r.Xml "own sliders go further")
    Check "the tab has the presets, the frame rate and the settings" ((Has $r.Xml 'opt_variable="RVK_Preset"') -and (Has $r.Xml 'opt_variable="AOVK_FpsCap"') -and (Has $r.Xml 'opt_variable="RVK_Enhance"'))
    Check "... and an Advanced tab with the sliders, not in the Renderer tab" ((([regex]::Matches($r.Xml, 'label="Advanced"')).Count -eq 1) -and ([regex]::Matches($r.Xml, 'opt_variable="RVK_Tess"')).Count -eq 1 -and ($r.Xml.IndexOf('opt_variable="RVK_Tess"') -gt $r.Xml.IndexOf('label="Advanced"')))
}

$r = Run "no-renderer" $false ""
if ($r) {
    Check "ao-vk runs" ($r.Log -match "ao-vk [0-9.]+: running")
    Check "it says the renderer isn't there" (Has $r.Log "[ao-vk] no renderer settings: ao-vk's renderer (randy31.dll) isn't installed")
    Check "the tab says so, with the frame rate cap" ((Has $r.Xml "isn&#39;t installed") -or (Has $r.Xml "isn't installed"))
    Check "no renderer settings in the tab" (-not (Has $r.Xml "RVK_"))
}

if (-not $Keep) { Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue }
if ($failures) { Write-Host "$failures check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "all checks passed" -ForegroundColor Green
