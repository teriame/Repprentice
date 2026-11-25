<#
  Builds the single self-contained Windows executable distribution of The Apprentice:

    dist\Apprentice-<version>-x64.exe
    dist\SHA256SUMS.txt

  Behaves like the Linux AppImage: a single standalone .exe with embedded
  recompiler & CHD tools, interactive GUI launcher, in-window file browser,
  in-game Quick Menu overlay, and GPU Cartoon HD / HD+ shader.  Neither the
  disc image nor any derived game code is included (--stub).  At runtime,
  everything written stays strictly inside CWD (.\saves\ and .\apprentice.ini)
  or %TEMP%.

  Usage:  powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-NoBuild]
#>
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$ver = (Get-Content (Join-Path $root 'VERSION') -TotalCount 1).Trim()
$name = "Apprentice-$ver"
$dist = Join-Path $root 'dist'
$builtExe = Join-Path $root 'build\cmake\Release\apprentice.exe'
$outExe = Join-Path $dist "$name-x64.exe"

if (-not $NoBuild) {
    Write-Host "Building zero-derived-code release $ver (--stub)..."
    & (Join-Path $root 'build.bat') --stub --compile-only
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}
if (-not (Test-Path $builtExe)) {
    throw "$builtExe is missing: run build.bat --stub"
}

New-Item -ItemType Directory -Force $dist | Out-Null
foreach ($old in 'apprentice.exe', 'apprentice_hd.exe') {
    $p = Join-Path $root $old
    if (Test-Path $p) { Remove-Item $p -Force }
}

Copy-Item $builtExe -Destination $outExe -Force
if (Test-Path (Join-Path $root 'gen')) { Remove-Item (Join-Path $root 'gen') -Recurse -Force }
if (Test-Path (Join-Path $root 'extracted')) { Remove-Item (Join-Path $root 'extracted') -Recurse -Force }
Write-Host "  $outExe"

$hash = (Get-FileHash $outExe -Algorithm SHA256).Hash.ToLower()
"$hash  $(Split-Path $outExe -Leaf)" | Set-Content -Encoding ascii (Join-Path $dist 'SHA256SUMS.txt')
Write-Host "Done: $outExe (single self-contained executable, zero derived game code)"
