# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Build a local, offline updater page for the firmware produced by
# sdk/Build.ps1 -FirmwareOnly. It uses the SDK's own updater (Update.html.in,
# WebUpdate.js) with the same package format as the official -Package build,
# and embeds the stock M-VAVE V15 image for rollback.
# The browser simulator is not included (the synth was removed).
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sdk = Join-Path $root 'sdk'
$build = Join-Path $root 'build/dx7'
$utf8 = New-Object Text.UTF8Encoding $false
function Hash($file) { (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }

$firmware = Join-Path $build 'generated/FM1_OFFICIAL_OPEN.fwsc'
$report = Join-Path $build 'firmware-report.json'
$identity = Join-Path $build 'build-identity.json'
foreach ($file in @($firmware, $report, $identity)) {
    if (!(Test-Path -LiteralPath $file)) { throw "Missing $file. Run build.bat first." }
}
if (!(Get-Content -LiteralPath $report -Raw | ConvertFrom-Json).passed) { throw 'Firmware checks did not pass.' }
$id = Get-Content -LiteralPath $identity -Raw | ConvertFrom-Json
$version = [regex]::Match([IO.File]::ReadAllText((Join-Path $root 'app/version.h')), '(?m)^#define FM1_RELEASE_VERSION "(V\d+)"').Groups[1].Value
$product = [regex]::Match([IO.File]::ReadAllText((Join-Path $root 'app/version.h')), '(?m)^#define FM1_UPDATE_PRODUCT "(FM-1_8\d{6})"').Groups[1].Value
if (!$version -or !$product -or $id.release_version -cne $version) { throw 'version.h does not match the built firmware.' }

# Stock V15 recovery image, as in Build.ps1 -Package.
$stockFile = Join-Path $sdk 'firmware/FM-1_015.fwsc'
$stockHash = 'db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a'
if ((Hash $stockFile) -cne $stockHash) { throw 'Stock V15 recovery firmware is missing or invalid.' }
$stock = [ordered]@{schema=1;kind='stock';product='FM-1_015';profile='M-VAVE Original V15';family=7;release_version='V15';
    build_id=$stockHash;sha256=$stockHash;bytes=(Get-Item $stockFile).Length;
    loader_sha256='ef1f2db89b011fa53e71df363f0122de0ea896618c53fb53e8da676322004541';
    data=[Convert]::ToBase64String([IO.File]::ReadAllBytes($stockFile))}
$package = [ordered]@{schema=1;product=$product;profile='dx7';family=7;release_version=$version;
    build_id=$id.build_id;sha256=(Hash $firmware);bytes=(Get-Item $firmware).Length;
    loader_sha256=(Hash (Join-Path $sdk 'tools/pack/ota.bin'));hardware_validated=$false;
    data=[Convert]::ToBase64String([IO.File]::ReadAllBytes($firmware));stock=$stock} | ConvertTo-Json -Depth 4 -Compress
# Placeholder simulator: the page only loads it if the preview tab is used.
$simulator = [ordered]@{schema=1;release_version=$version;sha256=('0' * 64);source_sha256=('0' * 64);
    sample_bank='none';data=''} | ConvertTo-Json -Compress

$web = [IO.File]::ReadAllText((Join-Path $sdk 'Update.html.in'))
$web = [regex]::Replace($web, '<a\b[^>]*href="/\*FM1_SOURCE_URL\*/"[^>]*>(.*?)</a>', '<span>$1 (local build)</span>')
$notices = @('FM-1 MIDI Processor (local build). APP: GPLv3 with the FM1 SDK linking exception; no warranty. SDK and firmware resources have separate terms in THIRD_PARTY.md.',
    'FM-1 V15 recovery firmware: https://www.m-vave.com/download (PC Firmware / FM-1 V15). Distributed by M-VAVE under Apache-2.0. See THIRD_PARTY.md.')
foreach ($file in @('THIRD_PARTY.md','LICENSE.md')) { $notices += "`n===== $file =====`n" + [IO.File]::ReadAllText((Join-Path $root $file)) }
$replacements = @{
    '/*FM1_SOURCE_URL*/'=''
    '/*FM1_PAYLOAD*/'=('const FM1_PACKAGE = ' + $package + ';')
    '/*FM1_SIMULATOR*/'=('const FM1_SIMULATOR = ' + $simulator + ';')
    '/*FM1_I18N*/'=[IO.File]::ReadAllText((Join-Path $sdk 'I18n.js'))
    '/*FM1_SCRIPT*/'=[IO.File]::ReadAllText((Join-Path $sdk 'WebUpdate.js'))
    '/*FM1_SIM_SCRIPT*/'=[IO.File]::ReadAllText((Join-Path $sdk 'Simulator.js'))
    '/*FM1_SAMPLE_SCRIPT*/'=[IO.File]::ReadAllText((Join-Path $sdk 'Samples.js'))
    '/*FM1_NOTICES*/'=[Net.WebUtility]::HtmlEncode(($notices -join "`n"))
}
foreach ($key in $replacements.Keys) { $web = $web.Replace($key, $replacements[$key]) }
if ($web -match '/\*FM1_[A-Z_]+\*/') { throw "Unexpanded placeholder: $($Matches[0])" }
# Open on the Device update tab. The header's "Connect device" button follows
# the active tab, and the default Play & edit tab connects to the Open
# project library, which this firmware and stock V15 do not support.
# Also skip the post-flash sample-bank check: it uses the Open project
# protocol, which this firmware does not implement, so it can only time out.
$startTab = '<script>addEventListener("load",()=>{globalThis.FM1EnsureSamples=async()=>{};' +
    'document.getElementById("mode-update")?.click();});</script>'
$end = $web.LastIndexOf('</html>')
if ($end -lt 0) { throw 'Updater template has no </html>.' }
$web = $web.Insert($end, $startTab + "`n")
$output = Join-Path $root 'build/updater/FM1-MidiProc-Updater.html'
[IO.Directory]::CreateDirectory((Split-Path $output)) | Out-Null
[IO.File]::WriteAllText($output, $web, $utf8)
Write-Host "Updater: $output"
Write-Host "Firmware build $($id.build_id.Substring(0,16)), sha256 $((Hash $firmware).Substring(0,16))"
