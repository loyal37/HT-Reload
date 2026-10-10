# Builds the shell (build\HTReload.asi) and the core (build\HTReload.core.dll), and with -Test the
# stand-in host used by test\run.ps1.
# Keep this file ASCII: Windows PowerShell 5.1 reads BOM-less scripts in the system code page.
param([switch]$Test)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ x64 toolset was not found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$out = Join-Path $root 'build'
$flags = "/nologo /utf-8 /std:c++20 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I`"$root\src`""

function Invoke-Cl([string]$objDir, [string]$arguments) {
    New-Item -ItemType Directory -Force $objDir | Out-Null
    # cl runs through a generated script so that the quoted paths survive cmd's own quoting rules.
    $script = Join-Path $objDir 'build.cmd'
    Set-Content -Encoding Ascii $script "@echo off`r`ncall `"$vcvars`" >nul 2>&1`r`ncl $flags /Fo`"$objDir/`" $arguments`r`nexit /b %errorlevel%"
    cmd /c $script
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
}

function Sources([string]$folder) {
    (Get-ChildItem (Join-Path $root $folder) -Filter *.cpp | ForEach-Object { '"' + $_.FullName + '"' }) -join ' '
}

Invoke-Cl (Join-Path $out 'obj\core') "/LD $(Sources 'src') /Fe`"$out\HTReload.core.dll`" /link /NOLOGO /INCREMENTAL:NO user32.lib"
Write-Host "Built $out\HTReload.core.dll"
Invoke-Cl (Join-Path $out 'obj\shell') "/LD $(Sources 'shell') /Fe`"$out\HTReload.asi`" /link /NOLOGO /INCREMENTAL:NO user32.lib"
Write-Host "Built $out\HTReload.asi"

if ($Test) {
    $hostDir = Join-Path $out 'test\HT\Binaries\Win64'
    New-Item -ItemType Directory -Force $hostDir | Out-Null
    Invoke-Cl (Join-Path $out 'test\obj') "`"$root\test\host.cpp`" `"$root\src\cityhash.cpp`" /Fe`"$hostDir\HTGame.exe`" /link /NOLOGO /INCREMENTAL:NO user32.lib"
    Write-Host "Built $hostDir\HTGame.exe"
}
