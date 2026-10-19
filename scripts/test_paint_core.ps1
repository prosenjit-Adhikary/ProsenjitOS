$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$paint = Join-Path $root 'user/apps/paint/paint_core.c'
$test = Join-Path $root 'tests/paint_core_test.c'
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("prosenjitos-paint-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null

try {
    $compiler = $null
    foreach ($candidate in @('cl.exe', 'clang.exe', 'gcc.exe', 'cc.exe', 'clang', 'gcc', 'cc')) {
        $found = Get-Command $candidate -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $compiler = $found; break }
    }
    if (-not $compiler) {
        throw 'No supported C compiler found on OfficeWindows (expected cl, clang, gcc, or cc).'
    }
    $compilerPath = $compiler.Source
    $name = [System.IO.Path]::GetFileName($compilerPath)
    $output = Join-Path $tmp 'paint_core_test.exe'
    Write-Host ("Paint C compiler: " + $compilerPath)

    Push-Location $tmp
    try {
        if ($name -ieq 'cl.exe') {
            & $compilerPath '/nologo' '/std:c11' '/W4' '/WX' '/O2' $paint $test ("/Fo:" + $tmp + "\") ("/Fe:" + $output)
        } else {
            & $compilerPath '-std=c11' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' $paint $test '-o' $output
        }
        if ($LASTEXITCODE -ne 0) {
            throw "Paint core compilation failed (exit code $LASTEXITCODE)."
        }
        & $output
        if ($LASTEXITCODE -ne 0) {
            throw "Paint core regression tests failed (exit code $LASTEXITCODE)."
        }
    } finally {
        Pop-Location
    }
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
