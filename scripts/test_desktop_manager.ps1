$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$desktop = Join-Path $root 'user/desktop'
$tests = Join-Path $root 'tests'
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("prosenjitos-dml-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null

function Import-VisualStudioEnvironment {
    # Self-hosted Windows runners may have Visual Studio installed without the
    # Developer Command Prompt. Discover vcvars64.bat and import its environment.
    $vswhereCandidates = @()
    if (\${env:ProgramFiles(x86)}) {
        $vswhereCandidates += (Join-Path \${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe')
    }
    if ($env:ProgramFiles) {
        $vswhereCandidates += (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
    }

    $vswhere = $vswhereCandidates |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1

    $vcvarsCandidates = @()
    if ($vswhere) {
        $installationPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installationPath) {
            $vcvarsCandidates += (Join-Path ([string]$installationPath.Trim()) 'VC\Auxiliary\Build\vcvars64.bat')
        }
    }

    foreach ($year in @('2022', '2019')) {
        foreach ($edition in @('BuildTools', 'Community', 'Professional', 'Enterprise')) {
            if (\${env:ProgramFiles(x86)}) {
                $base = Join-Path \${env:ProgramFiles(x86)} "Microsoft Visual Studio\$year\$edition"
                $vcvarsCandidates += (Join-Path $base 'VC\Auxiliary\Build\vcvars64.bat')
            }
        }
    }

    $vcvars = $vcvarsCandidates |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not $vcvars) { return $false }

    $cmdLine = 'call "' + $vcvars + '" >nul && set'
    $environmentLines = & $env:ComSpec /d /s /c $cmdLine
    if ($LASTEXITCODE -ne 0) {
        throw "Visual Studio environment initialization failed: $vcvars"
    }
    foreach ($line in $environmentLines) {
        # Ignore cmd.exe drive-current-directory entries such as '=C:=...'.
        if ($line -match '^([^=][^=]*)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
    return [bool](Get-Command 'cl.exe' -ErrorAction SilentlyContinue)
}

try {
    $bash = Get-Command bash -ErrorAction SilentlyContinue
    if ($bash) {
        & $bash.Source -n (Join-Path $root 'scripts/test_desktop_manager.sh')
        if ($LASTEXITCODE -ne 0) { throw 'Bash syntax validation failed for scripts/test_desktop_manager.sh.' }
        & $bash.Source -n (Join-Path $root 'scripts/desktop_complete.sh')
        if ($LASTEXITCODE -ne 0) { throw 'Bash syntax validation failed for scripts/desktop_complete.sh.' }
    } else {
        Write-Host 'bash is not installed; using the native Windows compiler path.'
    }

    $compiler = $null
    foreach ($candidate in @('cl.exe', 'clang.exe', 'gcc.exe', 'cc.exe', 'clang', 'gcc', 'cc')) {
        $found = Get-Command $candidate -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $compiler = $found; break }
    }

    if (-not $compiler) {
        Write-Host 'No compiler was found on PATH; searching installed Visual Studio C/C++ Build Tools.'
        if (Import-VisualStudioEnvironment) {
            $compiler = Get-Command 'cl.exe' -ErrorAction Stop | Select-Object -First 1
        }
    }
    if (-not $compiler) {
        $details = @(
            'No supported C compiler was found on OfficeWindows.',
            'Checked PATH for MSVC/Clang/GCC/cc and searched Visual Studio 2019/2022 C++ Build Tools.',
            'Install Visual Studio Build Tools with the “Desktop development with C++” workload on this runner, then rerun.'
        ) -join [Environment]::NewLine
        throw $details
    }

    Write-Host ("Using C compiler: " + $compiler.Source)
    $isMsvc = [System.IO.Path]::GetFileName($compiler.Source) -ieq 'cl.exe'
    $managerExe = Join-Path $tmp 'desktop_manager_test.exe'
    $runtimeExe = Join-Path $tmp 'desktop_runtime_test.exe'

    if ($isMsvc) {
        & $compiler.Source '/nologo' '/std:c11' '/W4' '/WX' '/O2' ("/I" + $desktop) (Join-Path $desktop 'desktop_manager.c') (Join-Path $tests 'desktop_manager_test.c') ("/Fo:" + $tmp + "\") ("/Fe:" + $managerExe)
        if ($LASTEXITCODE -ne 0) { throw 'MSVC compilation failed for desktop_manager_test.' }
        & $managerExe
        if ($LASTEXITCODE -ne 0) { throw "desktop_manager_test failed with exit code $LASTEXITCODE." }
        & $compiler.Source '/nologo' '/std:c11' '/W4' '/WX' '/O2' ("/I" + $desktop) (Join-Path $desktop 'desktop_manager.c') (Join-Path $desktop 'dml_runtime.c') (Join-Path $tests 'desktop_runtime_test.c') ("/Fo:" + $tmp + "\") ("/Fe:" + $runtimeExe)
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
