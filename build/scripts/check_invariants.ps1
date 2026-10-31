# =============================================================================
#  Architecture invariants for app923
#
#  Every rule below documents a decision that cost real debugging time once.
#  Run this before committing changes to vitis2/app923/src.
#
#    powershell -ExecutionPolicy Bypass -File tools\check_invariants.ps1
#
#  Exit code 0 = all invariants hold, 1 = at least one was violated.
#  ASCII only (Windows PowerShell 5.1 reads .ps1 as ANSI).
# =============================================================================

$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here "..")).Path
$src  = Join-Path $repo "vitis2\app923\src"

if (-not (Test-Path $src)) {
    Write-Host "source tree not found: $src"
    exit 2
}

$fail = 0

function Get-Files {
    param([string] $Sub, [string] $Filter)
    Get-ChildItem (Join-Path $src $Sub) -Recurse -File -Include $Filter -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\\.cache\\' }      # skip clangd index blobs
}

function Check-NoMatch {
    param([string] $Title, $Files, [string] $Pattern, [string] $Why, [switch] $CaseSensitive)
    $hits = @()
    foreach ($f in $Files) {
        $m = Select-String -Path $f.FullName -Pattern $Pattern -CaseSensitive:$CaseSensitive -ErrorAction SilentlyContinue
        foreach ($x in $m) { $hits += ("{0}:{1}: {2}" -f $f.Name, $x.LineNumber, $x.Line.Trim()) }
    }
    if ($hits.Count -eq 0) {
        Write-Host ("  OK    " + $Title)
    } else {
        Write-Host ("  FAIL  " + $Title)
        Write-Host ("        reason: " + $Why)
        $hits | Select-Object -First 8 | ForEach-Object { Write-Host ("        " + $_) }
        $script:fail++
    }
}

function Check-MustMatch {
    param([string] $Title, [string] $File, [string] $Pattern, [string] $Why)
    $m = Select-String -Path $File -Pattern $Pattern -ErrorAction SilentlyContinue
    if ($m) {
        Write-Host ("  OK    " + $Title)
    } else {
        Write-Host ("  FAIL  " + $Title)
        Write-Host ("        reason: " + $Why)
        $script:fail++
    }
}

Write-Host "== app923 architecture invariants =="
Write-Host ("source tree: " + $src)
Write-Host ""

# --- 1. the log lock must stay recursive -------------------------------------
Check-MustMatch "log lock is recursive (via OSAL)" (Join-Path $src "service\monitor_module\log\log.c") `
    "Osal_MutexCreateRecursive" `
    "a plain mutex deadlocks when a Log_Lock() region calls LOG_I() (see service/monitor)"

Check-NoMatch "log.c only talks to OSAL" @(Get-Item (Join-Path $src "service\monitor_module\log\log.c")) `
    "xSemaphore|vTask|xTask|FreeRTOS\.h" `
    "service/monitor_module/log/ is a cross-layer facility: it must go through osal/, not call the RTOS"

# --- 2. the SCU private timer belongs to FreeRTOS ----------------------------
Check-NoMatch "SCU timer untouched" (Get-Files "." "*.c") `
    "XScuTimer_" `
    "XILTIMER uses ps7_scutimer_0 as the FreeRTOS tick; touching it breaks the scheduler"

# --- 3. dependency direction: app -> service -> bsp -> Xilinx ----------------
#  EXCEPTION: the cross-layer log facility (service/monitor_module/log/log.h).
#  Logging is NOT a service component: every layer uses it (bsp prints register
#  state, service prints state machines, app prints command echo).  On 2026-10-16
#  log/ was moved under service/monitor_module/ together with led/ and monitor/,
#  so the bsp includes now LOOK like "a driver depending on service".
#  Only that one header is allowed; bsp still must not touch any real service
#  component (net / led / ls / pwm / ...).
Check-NoMatch "bsp does not include app or service" (Get-Files "bsp" "*.c*") `
    '#include\s+"(app|service)/(?!monitor_module/log/log\.h)' `
    "layering rule: drivers must not know about the layers above them (cross-layer log header is the only exception)"

Check-NoMatch "service does not include app" (Get-Files "service" "*.c*") `
    '#include\s+"app/' `
    "layering rule: app calls service, never the other way round"

Check-NoMatch "app does not include bsp" (Get-Files "app" "*.c*") `
    '#include\s+"bsp/' `
    "app must go through the service components (they own the bsp handles)"

#  CASE SENSITIVE on purpose (2026-10-16).  C is case sensitive, so a real driver
#  call is always spelled Led_SetMode / Pwm_Start / Ls_GetState -- matching those
#  exact names catches everything, and it no longer flags the ALL CAPS command
#  NAMES that the app now owns (app/monitor_app.c declares "LED_STA_0" / "LEDMASK",
#  and a case insensitive rule reads that as "Led_" + "S").
Check-NoMatch "app does not call driver APIs directly" (Get-Files "app" "*.c*") `
    '\b(Led_|Pwm_|Enet_|Ls_|Rs485_|PsSpi_)[A-Za-z]' `
    "app only uses Svc_* APIs: that is what keeps the drivers replaceable" `
    -CaseSensitive

Check-NoMatch "osal does not include other layers" (Get-Files "osal" "*.c*") `
    '#include\s+"(app|bsp|service|log)/' `
    "osal is the bottom of the software stack: it may only depend on the RTOS"

# --- 4. only osal_freertos.c may touch the RTOS ------------------------------
#  This is the measurable form of "the OSAL decouples us from FreeRTOS": if any
#  other file mentions an RTOS symbol, swapping the RTOS would no longer be a
#  one-file change.
$rtosPattern = 'xTaskCreate|xQueueCreate|xQueueSend|xQueueReceive|vTaskDelay|' +
               'vTaskStartScheduler|xTaskGetTickCount|xTaskGetSchedulerState|' +
               'xTaskNotifyGive|ulTaskNotifyTake|xTaskGetCurrentTaskHandle|' +
               'xSemaphore|xPortInstallInterruptHandler|vPortEnableInterrupt|' +
               'vPortDisableInterrupt|portYIELD_FROM_ISR|portMAX_DELAY|pdMS_TO_TICKS|' +
               'configTICK_RATE_HZ|#include\s+"(FreeRTOS|task|queue|semphr)\.h"'
$rtosHits = @()
foreach ($f in (Get-Files "." "*.c*")) {
    if ($f.Name -eq "osal_freertos.c") { continue }
    $m = Select-String -Path $f.FullName -Pattern $rtosPattern -ErrorAction SilentlyContinue
    foreach ($x in $m) {
        $rtosHits += ("{0}:{1}: {2}" -f $f.Name, $x.LineNumber, $x.Line.Trim())
    }
}
if ($rtosHits.Count -eq 0) {
    Write-Host "  OK    RTOS symbols only in osal/osal_freertos.c"
} else {
    Write-Host "  FAIL  RTOS symbols outside osal/osal_freertos.c"
    Write-Host "        reason: use osal.h (task/queue/mutex/time/interrupt); swapping the"
    Write-Host "                RTOS must stay a one-file change"
    $rtosHits | Select-Object -First 8 | ForEach-Object { Write-Host ("        " + $_) }
    $fail++
}

# --- 5. app layer must not touch registers ----------------------------------
Check-NoMatch "app layer has no register access" (Get-Files "app" "*.c") `
    "Xil_In32|Xil_Out32|XUartLite_|XGpio_|XGpioPs_|XScuTimer_|XScuGic_" `
    "everything below app/ goes through the service API"

# --- 6. logging goes through LOG_* -----------------------------------------
$allowedRaw = @("log.c", "enet_tcp.c")
$raw = @()
foreach ($f in (Get-Files "." "*.c")) {
    if ($allowedRaw -contains $f.Name) { continue }
    $m = Select-String -Path $f.FullName -Pattern 'xil_printf\s*\(' -ErrorAction SilentlyContinue
    foreach ($x in $m) { $raw += ("{0}:{1}: {2}" -f $f.Name, $x.LineNumber, $x.Line.Trim()) }
}
if ($raw.Count -eq 0) {
    Write-Host "  OK    logging goes through LOG_* (except log.c / enet_tcp.c lock groups)"
} else {
    Write-Host "  FAIL  raw xil_printf outside the allowed files"
    Write-Host "        reason: use LOG_I/LOG_W/LOG_E so lines stay tagged and untangled"
    $raw | Select-Object -First 8 | ForEach-Object { Write-Host ("        " + $_) }
    $fail++
}

# --- 7. the FSBL must initialise the PS exactly like the current hardware -----
#  Vitis copies ps7_init.c/.h into the FSBL app dir when the platform is created,
#  but does NOT refresh them when the platform changes.  zynq_fsbl/CMakeLists.txt
#  collects the APP dir copy, so a stale pair silently gives flash boot a different
#  PS init than JTAG: on 2026/10/04 it wrote GEM0_CLK_CTRL = EMIO/div1 instead of
#  IO PLL/div8 (125 MHz) and the Ethernet had a link but moved zero frames.
$fsblDir = Join-Path $repo "vitis2\soc_923\zynq_fsbl"
$artDir  = Join-Path $fsblDir "zynq_fsbl_bsp\hw_artifacts"
$stale = 0
foreach ($n in @("ps7_init.c", "ps7_init.h")) {
    $appCopy = Join-Path $fsblDir $n
    $handoff = Join-Path $artDir  $n
    if ((Test-Path $appCopy) -and (Test-Path $handoff)) {
        if ((Get-FileHash $appCopy -Algorithm MD5).Hash -ne (Get-FileHash $handoff -Algorithm MD5).Hash) {
            Write-Host ("  FAIL  FSBL " + $n + " is not the current hardware handoff")
            $stale++
        }
    }
}
if ($stale -eq 0) {
    Write-Host "  OK    FSBL ps7_init matches the current handoff"
} else {
    Write-Host "        reason: flash boot would initialise the PS differently from JTAG"
    Write-Host ("        fix:    copy vitis2\soc_923\zynq_fsbl\zynq_fsbl_bsp\hw_artifacts\ps7_init.*")
    Write-Host ("                over vitis2\soc_923\zynq_fsbl\ps7_init.*, then rebuild the FSBL")
    Write-Host "                and regenerate BOOT.bin (touch the files first: the copy keeps"
    Write-Host "                the old timestamp and ninja then says 'no work to do')"
    $fail++
}

# --- 8. the built boot image must carry that same value ----------------------
function Get-Gem0Value {
    param([string] $Path)
    # value written to 0xF8000140 (GEM0_CLK_CTRL): table entries are
    # { addr, mask, value, opcode(0x33) } as little-endian 32 bit words
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $s = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)
    $pat = ""
    foreach ($x in @(0x40, 0x01, 0x00, 0xF8, 0x71, 0x3F, 0xF0, 0x03)) { $pat += [char]$x }
    $found = @()
    $i = $s.IndexOf($pat)
    while ($i -ge 0) {
        $v = 0
        for ($k = 0; $k -lt 4; $k++) { $v = $v -bor ([int]$bytes[$i + 8 + $k] -shl (8 * $k)) }
        $found += $v
        $i = $s.IndexOf($pat, $i + 1)
    }
    return $found
}

$handoffC = Join-Path $artDir "ps7_init.c"
$want = $null
if (Test-Path $handoffC) {
    $m = Select-String -Path $handoffC -Pattern 'EMIT_MASKWRITE\(0XF8000140\s*,\s*0x[0-9A-Fa-f]+U?\s*,\s*0x([0-9A-Fa-f]+)' | Select-Object -First 1
    if ($m) { $want = [uint32]("0x" + $m.Matches[0].Groups[1].Value) }
}
$images = @(
    (Join-Path $fsblDir "build\fsbl.elf"),
    (Join-Path $repo "vitis2\app923\flash\BOOT.bin")
)
$imgBad = 0
foreach ($img in $images) {
    if (-not (Test-Path $img)) { continue }
    $vals = Get-Gem0Value $img
    $ok = ($vals.Count -gt 0 -and ($vals | Where-Object { $_ -ne $want }).Count -eq 0)
    if ($ok) {
        Write-Host ("  OK    " + (Split-Path $img -Leaf) + " carries GEM0_CLK_CTRL = 0x" + ("{0:X8}" -f $want))
    } else {
        Write-Host ("  FAIL  " + (Split-Path $img -Leaf) + " carries a stale GEM0_CLK_CTRL")
        Write-Host ("        want 0x" + ("{0:X8}" -f $want) + ", found " + (($vals | ForEach-Object { "0x" + ("{0:X8}" -f $_) }) -join ", "))
        Write-Host "        reason: rebuilt FSBL/BOOT.bin not regenerated after the ps7_init fix"
        $imgBad++
    }
}
if ($imgBad -gt 0) { $fail++ }

Write-Host ""
if ($fail -eq 0) {
    Write-Host "RESULT: all invariants hold"
    exit 0
}

Write-Host ("RESULT: " + $fail + " invariant(s) violated")
exit 1
