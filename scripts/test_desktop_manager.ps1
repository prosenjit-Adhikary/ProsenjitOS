$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$desktop = Join-Path $root 'user/desktop'
$tests = Join-Path $root 'tests'
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("prosenjitos-dml-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null

try {
    $bash = Get-Command bash -ErrorAction SilentlyContinue
    if ($bash) {
        & $bash.Source -n (Join-Path $root 'scripts/test_desktop_manager.sh')
        if ($LASTEXITCODE -ne 0) { throw 'Bash syntax validation failed for scripts/test_desktop_manager.sh.' }
    } else {
        Write-Host 'bash is not installed; using the native Windows compiler path.'
    }

    $compiler = $null
    foreach ($candidate in @('cl.exe', 'clang.exe', 'gcc.exe', 'cc.exe', 'clang', 'gcc', 'cc')) {
        $found = Get-Command $candidate -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $compiler = $found; break }
    }
    if (-not $compiler) {
        throw 'No supported C compiler was found on OfficeWindows (looked for MSVC cl, Clang, GCC, or cc). Install/configure a compiler on the designated runner, then rerun this job.'
    }

    Write-Host ("Using C compiler: " + $compiler.Source)
    $isMsvc = [System.IO.Path]::GetFileName($compiler.Source) -ieq 'cl.exe'
    $managerExe = Join-Path $tmp 'desktop_manager_test.exe'
    $runtimeExe = Join-Path $tmp 'desktop_runtime_test.exe'

    if ($isMsvc) {
        & $compiler.Source '/nologo' '/std:c11' '/W4' '/WX' '/O2' ("/I" + $desktop) (Join-Path $desktop 'desktop_manager.c') (Join-Path $tests 'desktop_manager_test.c') ("/Fe:" + $managerExe)
        if ($LASTEXITCODE -ne 0) { throw 'MSVC compilation failed for desktop_manager_test.' }
        & $managerExe
        if ($LASTEXITCODE -ne 0) { throw "desktop_manager_test failed with exit code $LASTEXITCODE." }
        & $compiler.Source '/nologo' '/std:c11' '/W4' '/WX' '/O2' ("/I" + $desktop) (Join-Path $desktop 'desktop_manager.c') (Join-Path $desktop 'dml_runtime.c') (Join-Path $tests 'desktop_runtime_test.c') ("/Fe:" + $runtimeExe)
        if ($LASTEXITCODE -ne 0) { throw 'MSVC compilation failed for desktop_runtime_test.' }
        & $runtimeExe
        if ($LASTEXITCODE -ne 0) { throw "desktop_runtime_test failed with exit code $LASTEXITCODE." }
    } else {
        & $compiler.Source '-std=c11' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' (('-I' + $desktop)) (Join-Path $desktop 'desktop_manager.c') (Join-Path $tests 'desktop_manager_test.c') '-o' $managerExe
        if ($LASTEXITCODE -ne 0) { throw 'Compilation failed for desktop_manager_test.' }
        & $managerExe
        if ($LASTEXITCODE -ne 0) { throw "desktop_manager_test failed with exit code $LASTEXITCODE." }
        & $compiler.Source '-std=c11' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' (('-I' + $desktop)) (Join-Path $desktop 'desktop_manager.c') (Join-Path $desktop 'dml_runtime.c') (Join-Path $tests 'desktop_runtime_test.c') '-o' $runtimeExe
        if ($LASTEXITCODE -ne 0) { throw 'Compilation failed for desktop_runtime_test.' }
        & $runtimeExe
        if ($LASTEXITCODE -ne 0) { throw "desktop_runtime_test failed with exit code $LASTEXITCODE." }
    }

    Write-Host 'DML manager and runtime unit tests passed.'
}
finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}