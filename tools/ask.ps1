# Sends commands to the plugin in the running game through its command file and prints the answers.
#   .\tools\ask.ps1 'objects GameEngine', 'names CollectGarbage'
# Keep this file ASCII: Windows PowerShell 5.1 reads BOM-less scripts in the system code page.
param(
    [Parameter(Mandatory)][string[]]$Lines,
    [string]$GameWin64 = 'D:\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64',
    [int]$TimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
if (-not (Get-Process HTGame -ErrorAction SilentlyContinue)) { throw 'The game is not running.' }

# The plugin keeps its files open for writing, so they have to be read with write sharing allowed.
function Read-Shared([string]$path) {
    if (-not (Test-Path $path)) { return '' }
    $stream = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite, Delete')
    try { (New-Object IO.StreamReader $stream, ([Text.Encoding]::UTF8)).ReadToEnd() } finally { $stream.Dispose() }
}

# The core logs a line with this marker after each command.
$done = 'Command done:'
function Count-Done { ([regex]::Matches((Read-Shared (Join-Path $GameWin64 'HTReload.log')), [regex]::Escape($done))).Count }

$wanted = (Count-Done) + @($Lines | Where-Object { $_.Trim() }).Count
[IO.File]::WriteAllText((Join-Path $GameWin64 'HTReload.cmd.txt'), (($Lines -join "`r`n") + "`r`n"))
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Count-Done) -lt $wanted) {
    if ((Get-Date) -gt $deadline) { throw 'Timed out waiting for the plugin to finish the commands.' }
    if (-not (Get-Process HTGame -ErrorAction SilentlyContinue)) { throw 'The game exited while the commands were running.' }
    Start-Sleep -Milliseconds 400
}
Start-Sleep -Milliseconds 300
Read-Shared (Join-Path $GameWin64 'HTReload.out.txt')
