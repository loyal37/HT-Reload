# End-to-end check against the stand-in host: a pak present at startup must be left alone and a
# container published afterwards must be mounted once, on the host's main thread.
# Keep this file ASCII: Windows PowerShell 5.1 reads BOM-less scripts in the system code page.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
& (Join-Path $root 'build.ps1') -Test

$testRoot = Join-Path $root 'build\test'
$win64 = Join-Path $testRoot 'HT\Binaries\Win64'
$paks = Join-Path $testRoot 'HT\Content\Paks'
if (Test-Path $paks) { Remove-Item -Recurse -Force $paks }
New-Item -ItemType Directory -Force (Join-Path $paks 'MOD\base'), (Join-Path $paks 'MOD\hot') | Out-Null
Set-Content (Join-Path $paks 'MOD\base\base_P.pak') 'startup pak'
Copy-Item (Join-Path $root 'build\HTReload.asi') $win64 -Force
$log = Join-Path $win64 'HTReload.log'
Remove-Item $log -ErrorAction SilentlyContinue

$stdout = Join-Path $testRoot 'host.out.txt'
$process = Start-Process (Join-Path $win64 'HTGame.exe') -WorkingDirectory $win64 -PassThru -NoNewWindow -RedirectStandardOutput $stdout
$null = $process.Handle  # keeps ExitCode available after the process ends

# The plugin keeps its log open for writing, so it has to be read with write sharing allowed.
function Read-Log {
    if (-not (Test-Path $log)) { return '' }
    $stream = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite, Delete')
    try { (New-Object IO.StreamReader $stream, ([Text.Encoding]::UTF8)).ReadToEnd() } finally { $stream.Dispose() }
}

# The plugin logs a line starting with these two characters once it is watching for new paks.
$ready = [string][char]0x5C31 + [char]0x7EEA
$deadline = (Get-Date).AddSeconds(30)
while (-not (Read-Log).Contains($ready)) {
    if ($process.HasExited -or (Get-Date) -gt $deadline) { throw 'The plugin never became ready.' }
    Start-Sleep -Milliseconds 300
}

# Publish the way the packager does: complete temporary files renamed into place one by one.
$hot = Join-Path $paks 'MOD\hot'
foreach ($extension in 'pak', 'utoc', 'ucas') {
    $temporary = Join-Path $hot "hot_1_P.$extension.tmp"
    Set-Content $temporary "container $extension"
    Rename-Item $temporary "hot_1_P.$extension"
    Start-Sleep -Milliseconds 200
}

if (-not $process.WaitForExit(40000)) { $process.Kill() }
$process.WaitForExit()
Write-Host '--- host output ---'
Get-Content $stdout
Write-Host '--- HTReload.log ---'
Read-Log
Write-Host "--- host exit code: $($process.ExitCode) ---"
if ($process.ExitCode -ne 0) { throw 'Test failed' }
Write-Host 'Test passed'
