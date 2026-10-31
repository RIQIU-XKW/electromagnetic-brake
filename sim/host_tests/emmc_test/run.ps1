# =============================================================================
#  eMMC (SD1 / FatFs) file-logic host test
#
#  Compiles the real bsp/emmc/emmc.c on the PC against an in-memory FatFs stub
#  (stub/ff.h + host_ff.c) and checks the record / read-back logic.
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

$exe = Join-Path $here "test_emmc.exe"

Write-Host "== eMMC file-logic host test =="
Write-Host ("source under test: " + (Join-Path $src "bsp\emmc\emmc.c"))
Write-Host ""

# NOTE: stub/ comes FIRST so our ff.h shadows third_party/xilffs/include/ff.h.
#       The real FatFs is NOT compiled here -- the stub stands in for it.
$gccArgs = @(
    "-std=c99", "-Wall", "-Wextra",
    "-I" + (Join-Path $here "stub"),
    "-I$src",
    (Join-Path $here "test_emmc.c"),
    (Join-Path $here "host_ff.c"),
    (Join-Path $here "host_osal.c"),
    (Join-Path $src "bsp\emmc\emmc.c"),
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
