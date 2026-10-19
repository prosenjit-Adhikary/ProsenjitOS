$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$paint = Join-Path $root 'user/apps/paint/paint_core.c'
$test = Join-Path $root 'tests/paint_core_test.c'
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("prosenjitos-paint-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null

try {
    $compiler = $null
    foreach ($candidate in @('clang.exe', 'gcc.exe', 'cc.exe', 'clang', 'gcc', 'cc', 'cl.exe')) {
        $found = Get-Command $candidate -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $compiler = $found; break }
    }
    $isZig = $false
    if (-not $compiler) {
        # Last-resort compiler: download official Zig and verify its pinned digest.
        $zigVersion = '0.17.0'
        $archiveName = "zig-x86_64-windows-$zigVersion.zip"
        $archive = Join-Path $tmp $archiveName
        $extract = Join-Path $tmp 'zig-extracted'
        $expectedSha256 = 'b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428'
        $url = "https://ziglang.org/download/$zigVersion/$archiveName"
        $downloaded = $false
        for ($attempt = 1; $attempt -le 4; $attempt++) {
            try {
                Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing -TimeoutSec 180 -ErrorAction Stop
                $downloaded = $true
                break
            } catch {
                Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
                if ($attempt -eq 4) { throw "Unable to download pinned Zig compiler: $($_.Exception.Message)" }
                Start-Sleep -Seconds ([Math]::Min(30, 5 * [Math]::Pow(2, $attempt - 1)))
            }
        }
        if (-not $downloaded) { throw 'Pinned Zig compiler download did not complete.' }
        $actualSha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualSha256 -ne $expectedSha256) { throw "Zig SHA-256 mismatch: $actualSha256" }
        Expand-Archive -LiteralPath $archive -DestinationPath $extract -Force
        $zig = Get-ChildItem -LiteralPath $extract -Filter 'zig.exe' -File -Recurse | Select-Object -First 1
        if (-not $zig) { throw 'Verified Zig archive did not contain zig.exe.' }
        $compiler = $zig
        $isZig = $true
    }
    $compilerPath = if ($compiler -is [System.IO.FileInfo]) { $compiler.FullName } else { $compiler.Source }
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
