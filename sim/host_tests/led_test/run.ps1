# =============================================================================
#  breath_led (4 LEDs) host test -- driver registers AND the real command chain
#
#  Compiles production code on the PC:
#     service/inter_module/net_cmd.c    command registry + dispatch
#     service/inter_module/net_text.c   text helpers
#     app/monitor_app.c                 LED commands (the app layer owns them)
#     service/monitor_module/led/service_led.c LED component (queue + task + snapshot)
#     bsp/led/led.c            breath_led IP driver
#  against host stubs:
#     host_regfile.c   AXI register file + log/xil_printf
#     host_osal.c      OSAL over pthreads (real queue + real task) + Svc_Net_Reply
#
#  Needs -lpthread (winpthreads) because the led task really runs in a thread.
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

$exe = Join-Path $here "test_led.exe"

# service/inter_module/service_net.h pulls in bsp/enet/enet.h, which embeds a full
# "struct netif" -- so the real lwIP headers have to be on the include path.
# They ship with the exported Vitis platform (lwip/netif.h + arch/cc.h).
# Our own stub/ directory is listed FIRST, so xil_io.h / xparameters.h still
# come from the host stubs.
$lwipInc = Join-Path $repo "vitis2\soc_923\export\soc_923\sw\freertos_ps7_cortexa9_0\include"

if (-not (Test-Path (Join-Path $lwipInc "lwip\netif.h"))) {
    Write-Host ("lwIP headers not found under " + $lwipInc)
    Write-Host "build the soc_923 platform first, or pass a different tree"
    exit 2
}

Write-Host "== breath_led 4-LED host test =="
Write-Host "sources under test:"
Write-Host ("  " + (Join-Path $src "bsp\led\led.c"))
Write-Host ("  " + (Join-Path $src "service\monitor_module\led\service_led.c"))
Write-Host ("  " + (Join-Path $src "app\monitor_app.c"))
Write-Host ("  " + (Join-Path $src "service\inter_module\net_cmd.c"))
Write-Host ("  " + (Join-Path $src "service\inter_module\net_text.c"))
Write-Host ""

$gccArgs = @(
    "-std=c99", "-Wall", "-Wextra",
    "-I" + (Join-Path $here "stub"),
    "-I$src",
    "-I" + (Join-Path $src "bsp\led"),
    "-I$lwipInc",
    (Join-Path $here "test_led.c"),
    (Join-Path $here "host_regfile.c"),
    (Join-Path $here "host_osal.c"),
    (Join-Path $src "bsp\led\led.c"),
    (Join-Path $src "service\monitor_module\led\service_led.c"),
    (Join-Path $src "app\monitor_app.c"),
    (Join-Path $src "service\inter_module\net_cmd.c"),
    (Join-Path $src "service\inter_module\net_text.c"),
    "-o", $exe,
    "-lpthread"
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
