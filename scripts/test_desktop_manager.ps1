$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$desktop = Join-Path $root 'user/desktop'
$tests = Join-Path $root 'tests'
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("prosenjitos-dml-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null

function Import-VisualStudioEnvironment {
    $programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    # Self-hosted Windows runners may have Visual Studio installed without the
    # Developer Command Prompt. Discover vcvars64.bat and import its environment.
    $vswhereCandidates = @()
    if ($programFilesX86) {
        $vswhereCandidates += (Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe')
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
            if ($programFilesX86) {
                $base = Join-Path $programFilesX86 "Microsoft Visual Studio\$year\$edition"
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
    $isZig = $false
    if (-not $compiler) {
        # Last-resort, user-mode toolchain: fetch the official Zig compiler into
        # this job's temporary directory and verify its pinned SHA-256 before use.
        # This avoids a paid hosted runner and does not require admin installation.
        $zigVersion = '0.17.0'
        $zigArchiveName = "zig-x86_64-windows-$zigVersion.zip"
        $zigUrl = "https://ziglang.org/download/$zigVersion/$zigArchiveName"
        $zigExpectedSha256 = 'b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428'
        $zigArchive = Join-Path $tmp $zigArchiveName
        $zigExtract = Join-Path $tmp 'zig-extracted'
        Write-Host "No installed compiler found; downloading pinned Zig $zigVersion from the official Zig site."
        Invoke-WebRequest -Uri $zigUrl -OutFile $zigArchive -UseBasicParsing
        $zigActualSha256 = (Get-FileHash -LiteralPath $zigArchive -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($zigActualSha256 -ne $zigExpectedSha256) {
            throw "Zig archive SHA-256 mismatch. Expected $zigExpectedSha256, got $zigActualSha256."
        }
        Expand-Archive -LiteralPath $zigArchive -DestinationPath $zigExtract -Force
        $zigExe = Get-ChildItem -LiteralPath $zigExtract -Filter 'zig.exe' -File -Recurse | Select-Object -First 1
        if (-not $zigExe) { throw 'Verified Zig archive did not contain zig.exe.' }
        $compiler = $zigExe
        $isZig = $true
    }

    Write-Host ("Using C compiler: " + $compiler.FullName)
    $isMsvc = [System.IO.Path]::GetFileName($compiler.FullName) -ieq 'cl.exe'
    $managerExe = Join-Path $tmp 'desktop_manager_test.exe'
    $runtimeExe = Join-Path $tmp 'desktop_runtime_test.exe'

    if ($isMsvc) {
        & $compiler.FullName '/nologo' '/std:c11' '/W4' '/WX' '/O2' ("/I" + $desktop) (Join-Path $desktop 'desktop_manager.c') (Join-Path $tests 'desktop_manager_test.c') ("/Fo:" + $tmp + "\") ("/Fe:" + $managerExe)
        if ($LASTEXITCODE -ne 0) { throw 'MSVC compilation failed for desktop_manager_test.' }
        & $managerExe
        if ($LASTEXITCODE -ne 0) { throw "desktop_manager_test failed with exit code $LASTEXITCODE." }
        & $compiler.FullName '/nologo' '/std:c11' '/W4' '/WX' '/O2' ("/I" + $desktop) (Join-Path $desktop 'desktop_manager.c') (Join-Path $desktop 'dml_runtime.c') (Join-Path $tests 'desktop_runtime_test.c') ("/Fo:" + $tmp + "\") ("/Fe:" + $runtimeExe)
        if ($LASTEXITCODE -ne 0) { throw 'MSVC compilation failed for desktop_runtime_test.' }
        & $runtimeExe
        if ($LASTEXITCODE -ne 0) { throw "desktop_runtime_test failed with exit code $LASTEXITCODE." }
    } elseif ($isZig) {
        & $compiler.FullName 'cc' '-std=c11' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' (('-I' + $desktop)) (Join-Path $desktop 'desktop_manager.c') (Join-Path $tests 'desktop_manager_test.c') '-o' $managerExe
        if ($LASTEXITCODE -ne 0) { throw 'Zig C compilation failed for desktop_manager_test.' }
        & $managerExe
        if ($LASTEXITCODE -ne 0) { throw "desktop_manager_test failed with exit code $LASTEXITCODE." }
        & $compiler.FullName 'cc' '-std=c11' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' (('-I' + $desktop)) (Join-Path $desktop 'desktop_manager.c') (Join-Path $desktop 'dml_runtime.c') (Join-Path $tests 'desktop_runtime_test.c') '-o' $runtimeExe
        if ($LASTEXITCODE -ne 0) { throw 'Zig C compilation failed for desktop_runtime_test.' }
        & $runtimeExe
        if ($LASTEXITCODE -ne 0) { throw "desktop_runtime_test failed with exit code $LASTEXITCODE." }
    } else {
        & $compiler.FullName '-std=c11' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' (('-I' + $desktop)) (Join-Path $desktop 'desktop_manager.c') (Join-Path $tests 'desktop_manager_test.c') '-o' $managerExe
        if ($LASTEXITCODE -ne 0) { throw 'Compilation failed for desktop_manager_test.' }
        & $managerExe
        if ($LASTEXITCODE -ne 0) { throw "desktop_manager_test failed with exit code $LASTEXITCODE." }
        & $compiler.FullName '-std=c11' '-Wall' '-Wextra' '-Werror' '-pedantic' '-O2' (('-I' + $desktop)) (Join-Path $desktop 'desktop_manager.c') (Join-Path $desktop 'dml_runtime.c') (Join-Path $tests 'desktop_runtime_test.c') '-o' $runtimeExe
        if ($LASTEXITCODE -ne 0) { throw 'Compilation failed for desktop_runtime_test.' }
        & $runtimeExe
        if ($LASTEXITCODE -ne 0) { throw "desktop_runtime_test failed with exit code $LASTEXITCODE." }
    }

    Write-Host 'DML manager and runtime unit tests passed.'
}
finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
