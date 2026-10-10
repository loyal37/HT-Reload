# Disassembles code bytes fetched with the plugin's `code` command, using only MSVC's own tools:
# the bytes are assembled into an object file with ml64 and listed with dumpbin.
#   .\tools\ask.ps1 'code 0x18aacd0 0x100' | .\tools\disasm.ps1
# Input lines look like "0x018aacd0  4889...". Addresses in the listing are RVAs of the game image.
# Keep this file ASCII: Windows PowerShell 5.1 reads BOM-less scripts in the system code page.
param([Parameter(ValueFromPipeline)][string[]]$Text)

begin { $all = @() }
process { $all += $Text }
end {
    $ErrorActionPreference = 'Stop'
    $lines = ($all -join "`n") -split "`r?`n" | Where-Object { $_ -match '^0x[0-9a-fA-F]+\s+[0-9a-fA-F]+\s*$' }
    if (-not $lines) { throw 'No code lines in the input.' }
    $base = [Convert]::ToInt64(($lines[0] -split '\s+')[0], 16)
    $hex = ($lines | ForEach-Object { ($_ -split '\s+')[1] }) -join ''

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $work = Join-Path $env:TEMP ('htreload-disasm-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force $work | Out-Null
    try {
        $bytes = for ($i = 0; $i -lt $hex.Length; $i += 2) { '0' + $hex.Substring($i, 2) + 'h' }
        $rows = for ($i = 0; $i -lt $bytes.Count; $i += 16) { ' db ' + (($bytes[$i..([Math]::Min($i + 15, $bytes.Count - 1))]) -join ',') }
        Set-Content -Encoding Ascii (Join-Path $work 'blob.asm') (@('_TEXT SEGMENT', 'blob PROC') + $rows + @('blob ENDP', '_TEXT ENDS', 'END'))
        Set-Content -Encoding Ascii (Join-Path $work 'run.cmd') "@echo off`r`ncall `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1`r`ncd /d `"$work`"`r`nml64 /nologo /c /Foblob.obj blob.asm >nul`r`ndumpbin /nologo /disasm:nobytes blob.obj"
        $listing = cmd /c (Join-Path $work 'run.cmd')
        # dumpbin numbers the code from zero; shift addresses to the RVAs they came from. That applies to
        # branch targets (a lone number) and RIP-relative operands (a number right after the bracket),
        # not to displacements off a register, immediates or segment-relative offsets.
        $shift = { param($m) '{0:X}' -f ([Convert]::ToInt64($m.Value, 16) + $base) }
        foreach ($line in $listing) {
            if ($line -match '^\s+([0-9A-F]{16}): (.*)$') {
                $address = [Convert]::ToInt64($matches[1], 16) + $base
                $rest = $matches[2]
                if ($rest -match '^\w+\s+[0-9A-F]{16}$') {
                    $rest = [regex]::Replace($rest, '[0-9A-F]{16}$', $shift)
                } else {
                    $rest = [regex]::Replace($rest, '(?<=(?<![gf]s:)\[)[0-9A-F]{16}(?=h\])', $shift)
                    $rest = [regex]::Replace($rest, '(?<![0-9A-F])0{8,}(?=[0-9A-F]+h)', '')
                }
                '{0:X8}: {1}' -f $address, $rest
            }
        }
    } finally {
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    }
}
