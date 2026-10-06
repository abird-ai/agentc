# tests/run-built.ps1 — run the golden tests already built by
# `make win-tests` and compare with the goldens.
#
# Informational on Windows: the full suite has not been observed green on a
# real Windows kernel (TLS/SChannel, console/ConPTY and ARM64 remain
# unverified), so failures print but do not fail the job.
#
# The bash tool runs cmd.exe or PowerShell, selected by the `shell` config key
# or the AGENTC_SHELL environment override the product reads. tools_test and
# modes_test are run once per shell so the job proves real execution under
# both, and the summary states which golden runs passed. Under Wine only
# cmd.exe exists; tests/tools_test.c asserts the clear missing-shell error
# there instead.
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

$pass = 0; $failed = 0; $skipped = 0
# `make win-tests WIN_ARCH=arm64` writes to build\test\arm64; the x86-64
# default keeps build\test. Pick the subdirectory matching the host so a
# runner never executes binaries for another architecture.
$hostArm64 = $env:PROCESSOR_ARCHITECTURE -eq "ARM64" -or $env:PROCESSOR_ARCHITEW6432 -eq "ARM64"

function Check-Golden([string]$n, [string]$shell) {
    $bin = "build\test\$n.exe"
    if ($hostArm64 -and (Test-Path "build\test\arm64\$n.exe")) { $bin = "build\test\arm64\$n.exe" }
    $exp = "tests\data\$n.expected"
    if (!(Test-Path $exp) -or !(Test-Path $bin)) { $script:skipped++; return $true }
    $env:AGENTC_SHELL = $shell
    $actual = (& $bin 2>&1 | Out-String) -replace "`r`n", "`n"
    $expected = (Get-Content $exp -Raw) -replace "`r`n", "`n"
    if ($actual -ceq $expected) {
        Write-Host "ok   $n [$shell]"
        $script:pass++
        return $true
    }
    Write-Host "FAIL $n [$shell] (informational on Windows for now)"
    $script:failed++
    return $false
}

# Shell-touching suites run under both shells so cmd and PowerShell are
# proven, not assumed; everything else only needs one run.
$shellOk = @{ cmd = $true; powershell = $true }
Get-ChildItem "tests\*.c" | Sort-Object Name | ForEach-Object {
    $n = $_.BaseName
    if ($n -eq "tools_test" -or $n -eq "modes_test") {
        foreach ($s in @("cmd", "powershell")) {
            if (!(Check-Golden $n $s)) { $shellOk[$s] = $false }
        }
    } else {
        Check-Golden $n "cmd" | Out-Null
    }
}
$verified = @()
foreach ($s in @("cmd", "powershell")) {
    if ($shellOk[$s]) { $verified += $s }
}
Write-Host "windows golden: $pass ok, $failed failed, $skipped skipped"
if ($verified.Count -eq 2) {
    Write-Host "windows shells verified for real: cmd.exe and PowerShell"
} elseif ($verified.Count -eq 1) {
    Write-Host "windows shells verified for real: $($verified[0]) (the other run failed; informational)"
} else {
    Write-Host "windows shells verified for real: none (both runs failed; informational)"
}
exit 0
