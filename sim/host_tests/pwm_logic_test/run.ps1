# =============================================================================
#  pwm_axi_ip driver host test (register level)
#
#  Compiles the real bsp/pwm/pwm.c on the PC against a stubbed AXI register file
#  (stub/xil_io.h + host_regfile.c) and checks the register encodings.
#
#  ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI.
# =============================================================================

param(
    [string] $Gcc = "D:\mingw64\bin\gcc.exe"
)

$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here "..\..")).Path
$src  = Join-Path $repo "vitis2\app923\src"

if (-not (Test-Path $Gcc)) {
    $alt = Get-Command gcc -ErrorAction SilentlyContinue
    if ($alt) { $Gcc = $alt.Source } else {
        Write-Host "gcc not found (pass -Gcc <path>)"
        exit 2
    }
}

$pwm = Join-Path $src "bsp\pwm\pwm.c"
$exe = Join-Path $here "test_pwm.exe"

Write-Host "== pwm_axi_ip driver host test =="
Write-Host "source under test : $pwm"
Write-Host ""

$gccArgs = @(
    "-std=c99", "-Wall", "-Wextra",
    "-I" + (Join-Path $here "stub"),
    "-I$src",
    "-I" + (Join-Path $src "bsp\pwm"),
    (Join-Path $here "test_pwm.c"),
    (Join-Path $here "host_regfile.c"),
    $pwm,
    "-o", $exe
)

& $Gcc $gccArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "compile failed"
    exit 2
}

try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }
& $exe
$rc = $LASTEXITCODE

Write-Host ""
if ($rc -eq 0) {
    Write-Host "RESULT: all checks passed"
} else {
    Write-Host "RESULT: FAILED (exit $rc)"
}
exit $rc
