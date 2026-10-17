# Builds the dedicated playtest exe (game on the left, per-room notes on the right) and gathers it with
# SDL3.dll and the assets in .\playtest (notes: playtest_notes.json, pictures: screenshots\ next to the exe).
$ErrorActionPreference = 'Stop'
& "$PSScriptRoot\Compile.ps1" -Config RelWithDebInfo   # (with the .pdb: a crash.log next to the exe then names the function)
$rel = Join-Path $PSScriptRoot 'build\RelWithDebInfo'
$out = Join-Path $PSScriptRoot 'playtest'
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item (Join-Path $rel 'dds520_playtest.exe') $out -Force
Copy-Item (Join-Path $rel 'SDL3.dll') $out -Force
Copy-Item (Join-Path $rel 'dds520_playtest.pdb') $out -Force
robocopy (Join-Path $PSScriptRoot 'assets') (Join-Path $out 'assets') /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
Set-Content (Join-Path $out 'jouer.bat') "@echo off`r`ncd /d %~dp0`r`nstart `"`" dds520_playtest.exe %*`r`n"
Write-Host "OK -> $out"
