# End-to-end check against the stand-in host:
#   - a pak present at startup is left alone and a container published afterwards is mounted once,
#     on the host's main thread, followed by a collection request;
#   - a probe requested through the command file finds what refers to the superseded package;
#   - replacing the core while the host runs brings up a new core that still answers commands and
#     does not mount the container a second time.
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
Get-ChildItem $win64 -Filter 'HTReload*' | Remove-Item -Force
Remove-Item (Join-Path $win64 'host.quit') -ErrorAction SilentlyContinue
Copy-Item (Join-Path $root 'build\HTReload.asi'), (Join-Path $root 'build\HTReload.core.dll') $win64 -Force
$log = Join-Path $win64 'HTReload.log'
$report = Join-Path $win64 'HTReload.probe.txt'
$answers = Join-Path $win64 'HTReload.out.txt'

$stdout = Join-Path $testRoot 'host.out.txt'
$process = Start-Process (Join-Path $win64 'HTGame.exe') -WorkingDirectory $win64 -PassThru -NoNewWindow -RedirectStandardOutput $stdout
$null = $process.Handle  # keeps ExitCode available after the process ends

# The plugin keeps its log open for writing, so it has to be read with write sharing allowed.
function Read-Shared([string]$path) {
    if (-not (Test-Path $path)) { return '' }
    $stream = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite, Delete')
    try { (New-Object IO.StreamReader $stream, ([Text.Encoding]::UTF8)).ReadToEnd() } finally { $stream.Dispose() }
}

function Wait-Log([string]$what, [scriptblock]$condition, [int]$seconds = 30) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while (-not (& $condition (Read-Shared $log))) {
        if ($process.HasExited -or (Get-Date) -gt $deadline) {
            Write-Host (Read-Shared $log)
            throw "Timed out waiting for: $what"
        }
        Start-Sleep -Milliseconds 300
    }
}

function Count([string]$text, [string]$part) { ([regex]::Matches($text, [regex]::Escape($part))).Count }

function Send-Command([string]$line) {
    $before = Count (Read-Shared $log) $line
    [IO.File]::WriteAllText((Join-Path $win64 'HTReload.cmd.txt'), "$line`r`n")
    # Each command is logged once when it starts and once when it is done.
    $deadline = (Get-Date).AddSeconds(30)
    while ((Count (Read-Shared $log) $line) -lt $before + 2) {
        if ($process.HasExited -or (Get-Date) -gt $deadline) {
            Write-Host (Read-Shared $log)
            throw "Timed out waiting for command: $line"
        }
        Start-Sleep -Milliseconds 300
    }
}

# The core logs a line starting with these two characters once it is watching for new paks.
$ready = [string][char]0x5C31 + [char]0x7EEA
Wait-Log 'the core to become ready' { param($text) $text.Contains($ready) }

# Publish the way the packager does: complete temporary files renamed into place one by one.
$hot = Join-Path $paks 'MOD\hot'
foreach ($extension in 'pak', 'utoc', 'ucas') {
    $temporary = Join-Path $hot "hot_1_P.$extension.tmp"
    if ($extension -eq 'utoc') {
        # A real table of contents naming the one package the stand-in host has loaded.
        & (Join-Path $win64 'HTGame.exe') --write-utoc $temporary
        if ($LASTEXITCODE -ne 0) { throw 'Could not write the test container index.' }
    } else {
        Set-Content $temporary "container $extension"
    }
    Rename-Item $temporary "hot_1_P.$extension"
    Start-Sleep -Milliseconds 200
}
Wait-Log 'the container to be mounted' { param($text) $text.Contains('IPakFile') }

Send-Command 'probe manual hot'
$probe = if (Test-Path $report) { Get-Content $report -Raw -Encoding UTF8 } else { '' }

# Replace the core while the host keeps running.
Start-Sleep -Seconds 2
Copy-Item (Join-Path $root 'build\HTReload.core.dll') (Join-Path $win64 'HTReload.core.dll') -Force
(Get-Item (Join-Path $win64 'HTReload.core.dll')).LastWriteTime = Get-Date
Wait-Log 'the replaced core to become ready' { param($text) (Count $text $ready) -ge 2 }
Send-Command 'objects T_Body'
$objects = Read-Shared $answers

Set-Content (Join-Path $win64 'host.quit') 'quit'
if (-not $process.WaitForExit(20000)) { $process.Kill() }
$process.WaitForExit()
Write-Host '--- host output ---'
Get-Content $stdout
Write-Host '--- HTReload.log ---'
Write-Host (Read-Shared $log)
Write-Host '--- HTReload.probe.txt ---'
Write-Host $probe
Write-Host '--- HTReload.out.txt ---'
Write-Host $objects
Write-Host "--- host exit code: $($process.ExitCode) ---"
if ($process.ExitCode -ne 0) { throw 'Test failed' }

# The probe must find the loaded texture, its root flag, the object that points at it directly and
# the object that holds it through a heap block, and must not blame unrelated objects.
foreach ($expected in 'Texture2D /Game/Characters/Test/T_Body.T_Body', '[RootSet]',
                      'Keeper /Game/Other.DirectKeeper +0x30 -> T', 'Keeper /Game/Other.ArrayKeeper +0x38 -> ') {
    if (-not $probe.Contains($expected)) { throw "Probe report lacks: $expected" }
}
if ($probe.Contains('Filler')) { throw 'Probe report blames an unrelated object.' }
if (-not $objects.Contains('Texture2D  /Game/Characters/Test/T_Body.T_Body')) { throw 'The replaced core did not answer the objects query.' }
Write-Host 'Test passed'
