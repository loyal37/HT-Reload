# Copies build\HTReload.asi next to the game's ASI loader (dsound.dll).
# Keep this file ASCII: Windows PowerShell 5.1 reads BOM-less scripts in the system code page.
param([string]$GameWin64 = 'D:\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64')

$ErrorActionPreference = 'Stop'
$asi = Join-Path $PSScriptRoot 'build\HTReload.asi'
if (-not (Test-Path $asi)) { throw 'Nothing to deploy yet. Run build.ps1 first.' }
if (-not (Test-Path (Join-Path $GameWin64 'HTGame.exe'))) { throw "Not the game's Win64 folder: $GameWin64" }
if (Get-Process HTGame -ErrorAction SilentlyContinue) { throw 'The game is running. Close it before deploying.' }

$target = Join-Path $GameWin64 'HTReload.asi'
Copy-Item $asi $target -Force
Write-Host "Deployed $target"
Write-Host "SHA256 $((Get-FileHash $target -Algorithm SHA256).Hash)"
