# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Hikari
[CmdletBinding()]
param([switch]$Clean)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$upstreamRoot = Join-Path $taskRoot 'upstream\fxsound-app'
$buildRoot = Join-Path $taskRoot 'build'
$copyRoot = Join-Path $buildRoot 'upstream'
$outputRoot = Join-Path $taskRoot 'dist'
$pin = 'd8e7a23d37ed5939c2a3090a1c1756c7f2500b17'
if (!('HikariBuildProcessExitCode' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class HikariBuildProcessExitCode {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
}
'@
}

function Invoke-Checked {
    param([string]$Executable, [string[]]$Arguments)
    $global:LASTEXITCODE = -987654321
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE" }
}

Push-Location $taskRoot
try {
    $head = & git -C $upstreamRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $head.Trim() -ne $pin) { throw 'Upstream pin does not match' }
    $shallow = & git -C $upstreamRoot rev-parse --is-shallow-repository
    if ($LASTEXITCODE -ne 0 -or $shallow.Trim() -ne 'false') { throw 'Full upstream history is required' }
    $dirty = & git -C $upstreamRoot status --porcelain
    if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'Upstream must remain clean' }
    New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
    if (Test-Path -LiteralPath $copyRoot) {
        if ([IO.Path]::GetFullPath($copyRoot) -ne (Join-Path $taskRoot 'build\upstream')) { throw 'Invalid copy path' }
        $links = Get-ChildItem -LiteralPath $copyRoot -Recurse -Force -Attributes ReparsePoint
        if ($links) { throw 'Refusing to remove a source copy containing reparse points' }
        Remove-Item -LiteralPath $copyRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $copyRoot -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $upstreamRoot 'dsp') -Destination $copyRoot -Recurse
    Copy-Item -LiteralPath (Join-Path $upstreamRoot 'audiopassthru') -Destination $copyRoot -Recurse
    foreach ($patch in Get-ChildItem -LiteralPath (Join-Path $taskRoot 'patches') -Filter '*.patch' | Sort-Object Name) {
        $patchIsolation = Join-Path $buildRoot 'patch-isolation-not-a-repository'
        if (Test-Path -LiteralPath $patchIsolation) { throw 'Patch isolation path must not exist' }
        Invoke-Checked 'git' @("--git-dir=$patchIsolation", 'apply', '--no-index', '--check', '--unsafe-paths', '--directory=build/upstream', $patch.FullName)
        Invoke-Checked 'git' @("--git-dir=$patchIsolation", 'apply', '--no-index', '--unsafe-paths', '--directory=build/upstream', $patch.FullName)
    }
    Invoke-Checked 'node' @('tools/verify-upstream.mjs')
    $cmake = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
    if (!$cmake) {
        $cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    }
    if (!(Test-Path -LiteralPath $cmake)) { throw 'CMake is required' }
    Invoke-Checked $cmake @('-S', $taskRoot, '-B', (Join-Path $buildRoot 'cmake'), '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'v143,version=14.44.35207')
    $buildArguments = @('--build', (Join-Path $buildRoot 'cmake'), '--config', 'Release', '--parallel')
    if ($Clean) { $buildArguments += '--clean-first' }
    Invoke-Checked $cmake $buildArguments
    $exe = Join-Path $buildRoot 'cmake\Release\HikariSoundEngine.exe'
    $buildVectors = Join-Path $buildRoot 'cmake\Release\test-vectors'
    New-Item -ItemType Directory -Path $buildVectors -Force | Out-Null
    foreach ($vectorFile in @('eq-vectors.json', 'graphic-vectors.json')) {
        Copy-Item -LiteralPath (Join-Path $taskRoot "tests\$vectorFile") -Destination (Join-Path $buildVectors $vectorFile) -Force
    }
    $selfTestOutput = Join-Path $buildRoot 'self-test.stdout.txt'
    $selfTestError = Join-Path $buildRoot 'self-test.stderr.txt'
    $selfTestProcess = Start-Process -FilePath $exe -ArgumentList '--self-test' -WindowStyle Hidden -PassThru -RedirectStandardOutput $selfTestOutput -RedirectStandardError $selfTestError
    $selfTestHandle = $selfTestProcess.Handle
    if (!$selfTestProcess.WaitForExit(120000)) {
        Stop-Process -Id $selfTestProcess.Id -Force
        throw 'Offline self-test exceeded 120 seconds'
    }
    [uint32]$selfTestExitCode = 0
    if (![HikariBuildProcessExitCode]::GetExitCodeProcess($selfTestHandle, [ref]$selfTestExitCode) -or $selfTestExitCode -eq 259) {
        throw 'Offline self-test exit status is unavailable'
    }
    Get-Content -LiteralPath $selfTestOutput -Encoding UTF8
    if ($selfTestExitCode -ne 0) {
        Get-Content -LiteralPath $selfTestError -Encoding UTF8
        throw "Offline self-test failed: $selfTestExitCode"
    }
    $selfTestProcess.Dispose()
    New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
    Copy-Item -LiteralPath $exe -Destination (Join-Path $outputRoot 'HikariSoundEngine.exe') -Force
    $distributionVectors = Join-Path $outputRoot 'test-vectors'
    New-Item -ItemType Directory -Path $distributionVectors -Force | Out-Null
    foreach ($vectorFile in @('eq-vectors.json', 'graphic-vectors.json')) {
        Copy-Item -LiteralPath (Join-Path $buildVectors $vectorFile) -Destination (Join-Path $distributionVectors $vectorFile) -Force
    }
    $driverOutput = Join-Path $outputRoot 'drivers'
    New-Item -ItemType Directory -Path $driverOutput -Force | Out-Null
    foreach ($name in @('fxvad.sys', 'fxvad.inf', 'fxvadntamd64.cat')) {
        Copy-Item -LiteralPath (Join-Path $upstreamRoot "Installer\Drivers\Version14\win10\x64\$name") -Destination (Join-Path $driverOutput $name) -Force
    }
    # The repository stores fxvad.inf with LF, but the Microsoft-signed catalog covers its CRLF form.
    # Windows refuses a package whose INF is not in its catalog, so restore exactly the signed bytes.
    $infPath = Join-Path $driverOutput 'fxvad.inf'
    $infSource = [IO.File]::ReadAllBytes($infPath)
    if ($infSource -contains [byte]13) { throw 'Upstream fxvad.inf already contains CR; review the line ending restoration' }
    $infBytes = New-Object 'System.Collections.Generic.List[byte]'
    foreach ($value in $infSource) {
        if ($value -eq 10) { $infBytes.Add([byte]13) }
        $infBytes.Add($value)
    }
    [IO.File]::WriteAllBytes($infPath, $infBytes.ToArray())
    Copy-Item -LiteralPath (Join-Path $upstreamRoot 'LICENSE') -Destination (Join-Path $outputRoot 'LICENSE-AGPL-3.0.txt') -Force
    foreach ($document in @('THIRD-PARTY.md', 'SOURCE.txt', 'LICENSE-MS-LPL.txt', 'LICENSE-MS-LPL.rtf')) {
        $sourceDocument = Join-Path $taskRoot $document
        if (!(Test-Path -LiteralPath $sourceDocument)) { throw "Missing redistribution document: $document" }
        $destinationDocument = if ($document -eq 'THIRD-PARTY.md') { Join-Path $outputRoot 'THIRD-PARTY.txt' } else { Join-Path $outputRoot $document }
        Copy-Item -LiteralPath $sourceDocument -Destination $destinationDocument -Force
    }
    Invoke-Checked 'node' @('tools/verify-distribution.mjs')
    $hashes = foreach ($file in Get-ChildItem -LiteralPath $outputRoot -File -Recurse | Where-Object { $_.Name -ne 'SHA256SUMS.txt' } | Sort-Object FullName) {
        $relative = $file.FullName.Substring($outputRoot.Length + 1).Replace('\', '/')
        $hashStream = [IO.File]::OpenRead($file.FullName)
        $hashAlgorithm = [Security.Cryptography.SHA256]::Create()
        try {
            $digest = [BitConverter]::ToString($hashAlgorithm.ComputeHash($hashStream)).Replace('-', '').ToLowerInvariant()
        } finally { $hashAlgorithm.Dispose(); $hashStream.Dispose() }
        '{0}  {1}' -f $digest, $relative
    }
    [IO.File]::WriteAllText((Join-Path $outputRoot 'SHA256SUMS.txt'), (($hashes -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
    Write-Host 'Build and offline self-test passed. Drivers were copied unchanged except fxvad.inf, restored to its signed CRLF form and verified against the catalog.'
} finally { Pop-Location }
