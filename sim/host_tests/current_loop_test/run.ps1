# =============================================================================
#  Current loop host test (5 independent PIDs)
#
#  Compiles the real service_pid.c / service_current_loop.c on the PC against
#  stubbed ADS feedback, PWM output and OSAL, then drives the 10 ms control task
#  one cycle at a time and checks:
#    - per-bridge duty limits 37 / 40 / 28 / 45 / 75 %
#    - the slew rate on the duty increment
#    - the float integral and the conditional-integration anti-windup
#    - the shutdown ramp (target 0 -> duty back to 0 -> bridges off)
#    - the external-stop interlock (0xA5 / overcurrent / back to duty mode)
#    - the 100 ns default dead time at engage, and that a manual 0xDC frame
#      is not overwritten by the running loop
#
#  ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI, so non-ASCII bytes in
#  here can break parsing (a mangled ')' can even end the array literal early).
# =============================================================================

param(
    [string] $Gcc = "D:\mingw64\bin\gcc.exe"
)

# Do NOT use "Stop" here: gcc writes warnings to stderr, and "Stop" would abort
# the whole script after a harmless warning - the test would silently not run.
$ErrorActionPreference = "Continue"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here "..\..")).Path
$src  = Join-Path $repo "vitis2\app923\src"
$stub = Join-Path $here "stub"

if (-not (Test-Path $Gcc)) {
    $alt = Get-Command gcc -ErrorAction SilentlyContinue
    if ($alt) { $Gcc = $alt.Source } else {
        Write-Host "gcc not found (pass -Gcc <path>)"
        exit 2
    }
}

$pid_c  = Join-Path $src "service\Algorithm_module\service_pid.c"
$loop_c = Join-Path $src "service\Algorithm_module\service_current_loop.c"
$exe    = Join-Path $here "test_current_loop.exe"

Write-Host "== current loop host test =="
Write-Host "sources under test :"
Write-Host "  $pid_c"
Write-Host "  $loop_c"
Write-Host ""

$gccArgs = @(
    "-std=c99", "-Wall", "-Wextra",
    "-I$here",
    "-I$stub",
    "-I$src",
    (Join-Path $here "test_current_loop.c"),
    (Join-Path $here "host_stubs.c"),
    $pid_c,
    $loop_c,
    "-lm",
    "-o", $exe
)

& $Gcc $gccArgs
$compileRc = $LASTEXITCODE

if (($compileRc -ne 0) -or (-not (Test-Path $exe))) {
    Write-Host "args:"
    $gccArgs | ForEach-Object { Write-Host "  [$_]" }
    Write-Host "compile failed (exit $compileRc)"
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
