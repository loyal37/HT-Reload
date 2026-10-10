# Copies the plugin next to the game's ASI loader (dsound.dll).
# The core can be replaced while the game runs: the shell notices and swaps it in. The shell itself
# is locked by the running game, so a changed shell needs the game closed.
# Keep this file ASCII: Windows PowerShell 5.1 reads BOM-less scripts in the system code page.
param([string]$GameWin64 = 'D:\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64')

$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot 'build'
$shell = Join-Path $build 'HTReload.asi'
$core = Join-Path $build 'HTReload.core.dll'
if (-not ((Test-Path $shell) -and (Test-Path $core))) { throw 'Nothing to deploy yet. Run build.ps1 first.' }
if (-not (Test-Path (Join-Path $GameWin64 'HTGame.exe'))) { throw "Not the game's Win64 folder: $GameWin64" }

function Hash([string]$path) { if (Test-Path $path) { (Get-FileHash $path -Algorithm SHA256).Hash } else { '' } }

$running = [bool](Get-Process HTGame -ErrorAction SilentlyContinue)
$shellTarget = Join-Path $GameWin64 'HTReload.asi'
if ((Hash $shell) -ne (Hash $shellTarget)) {
    if ($running) { throw 'The shell changed and the game is running. Close the game before deploying.' }
    Copy-Item $shell $shellTarget -Force
    Write-Host "Deployed shell  $(Hash $shellTarget)"
} else {
    Write-Host 'Shell unchanged.'
}

# Written under another name first so that the shell never sees a half-copied file.
$coreTarget = Join-Path $GameWin64 'HTReload.core.dll'
$incoming = "$coreTarget.incoming"
Copy-Item $core $incoming -Force
Move-Item $incoming $coreTarget -Force
Write-Host "Deployed core   $(Hash $coreTarget)$(if ($running) { ' (the running game will swap it in within a few seconds)' })"
