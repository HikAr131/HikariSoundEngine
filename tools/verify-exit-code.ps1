# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Hikari
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Executable)
$ErrorActionPreference = 'Stop'
if (!('HikariExitCodeTest' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class HikariExitCodeTest {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
}
'@
}
$taskScratch = Join-Path $PSScriptRoot '..\build'
foreach ($testCase in @(@('--version', 0), @('--invalid-option-for-exit-test', 2))) {
    $taskStdout = Join-Path $taskScratch 'exit-code-test.stdout.txt'
    $taskStderr = Join-Path $taskScratch 'exit-code-test.stderr.txt'
    $taskProcess = Start-Process -FilePath $Executable -ArgumentList $testCase[0] -WindowStyle Hidden -PassThru -RedirectStandardOutput $taskStdout -RedirectStandardError $taskStderr
    $taskHandle = $taskProcess.Handle
    if (!$taskProcess.WaitForExit(15000)) {
        Stop-Process -Id $taskProcess.Id -Force
        throw 'Exit-code test timed out'
    }
    [uint32]$taskExit = 259
    if (![HikariExitCodeTest]::GetExitCodeProcess($taskHandle, [ref]$taskExit) -or $taskExit -ne [uint32]$testCase[1]) {
        throw "Exit-code test failed: expected $($testCase[1]), actual $taskExit"
    }
    $taskProcess.Dispose()
    Write-Host "Verified native process exit status: $taskExit"
}
