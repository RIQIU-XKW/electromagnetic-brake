# =============================================================================
#  Refresh the FSBL's ps7_init.c/.h from the current hardware handoff, rebuild the
#  FSBL and regenerate the boot image.
#
#  Why this is needed:
#    zynq_fsbl/CMakeLists.txt does "collect (PROJECT_LIB_SOURCES ps7_init.c)",
#    which takes the copy in the FSBL APP directory.  Regenerating the platform
#    from an XSA updates the hardware handoff (vitis2/soc_923/hw/sdt) and the BSP
#    (zynq_fsbl_bsp/hw_artifacts) and the debugger's _ide/psinit/ps7_init.tcl, but
#    NOT the FSBL app copy.  So after a hardware change the FSBL keeps
#    initialising the PS with an old register set while JTAG uses the new one.
#    On 2026/10/04 that wrote GEM0_CLK_CTRL = EMIO/div1 instead of IO PLL/div8:
#    the PHY had a link, MDIO worked, and not a single frame got through.
#
#  Usage (after ANY hardware change, then flash the regenerated BOOT.bin):
#    powershell -ExecutionPolicy Bypass -File tools\refresh_fsbl_psinit.ps1
#    powershell -ExecutionPolicy Bypass -File tools\refresh_fsbl_psinit.ps1 -Force
#
#  ASCII only (Windows PowerShell 5.1 reads .ps1 as ANSI).
# =============================================================================
param(
    [switch] $Force        # rebuild + regenerate even when nothing changed
)

$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here "..")).Path

$fsblDir   = Join-Path $repo "vitis2\soc_923\zynq_fsbl"
$artDir    = Join-Path $fsblDir "zynq_fsbl_bsp\hw_artifacts"
$fsblElf   = Join-Path $fsblDir "build\fsbl.elf"
$exportElf = Join-Path $repo "vitis2\soc_923\export\soc_923\sw\boot\fsbl.elf"
$bif       = Join-Path $repo "vitis2\app923\flash\untitled.bif"
$bootBin   = Join-Path $repo "vitis2\app923\flash\BOOT.bin"

$ninja   = "D:\vivado\Vitis\2024.2\tps\win64\lopper-1.1.0-packages\min_sdk\usr\bin\ninja.exe"
$bootgen = "D:\vivado\Vitis\2024.2\bin\bootgen.bat"
$gccBin  = "D:\vivado\Vitis\2024.2\gnu\aarch32\nt\gcc-arm-none-eabi\bin"

# --- the value written to 0xF8000140 (GEM0_CLK_CTRL) in a ps7_init table -------
# table entries are { addr, mask, value, opcode(0x33) } little endian 32 bit words
function Get-Gem0Value {
    param([string] $Path)
    if (-not (Test-Path $Path)) { return @() }
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $s = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)
    $pat = ""
    foreach ($x in @(0x40, 0x01, 0x00, 0xF8, 0x71, 0x3F, 0xF0, 0x03)) { $pat += [char]$x }
    $found = @()
    $i = $s.IndexOf($pat)
    while ($i -ge 0) {
        $v = 0
        for ($k = 0; $k -lt 4; $k++) { $v = $v -bor ([int]$bytes[$i + 8 + $k] -shl (8 * $k)) }
        $found += ('0x{0:X8}' -f $v)
        $i = $s.IndexOf($pat, $i + 1)
    }
    return $found
}

function Get-Gem0FromSource {
    param([string] $Path)
    $m = Select-String -Path $Path -Pattern 'EMIT_MASKWRITE\(0XF8000140\s*,\s*0x[0-9A-Fa-f]+U?\s*,\s*0x([0-9A-Fa-f]+)' |
         Select-Object -First 1
    if ($m) { return ('0x{0:X8}' -f [uint32]("0x" + $m.Matches[0].Groups[1].Value)) }
    return "<not found>"
}

Write-Host "== refresh FSBL ps7_init from the current hardware handoff =="

foreach ($p in @($fsblDir, $artDir, $bif)) {
    if (-not (Test-Path $p)) { Write-Host ("missing: " + $p); exit 2 }
}

# --- 1. refresh the two files the FSBL actually compiles ----------------------
$changed = 0
foreach ($n in @("ps7_init.c", "ps7_init.h")) {
    $appCopy = Join-Path $fsblDir $n
    $handoff = Join-Path $artDir  $n
    $same = $true
    if (Test-Path $appCopy) {
        $same = ((Get-FileHash $appCopy -Algorithm MD5).Hash -eq (Get-FileHash $handoff -Algorithm MD5).Hash)
    } else {
        $same = $false
    }
    if ($same) {
        Write-Host ("  up to date : " + $n)
    } else {
        if (Test-Path $appCopy) {
            $bak = $appCopy + ".bak_" + (Get-Date -Format "yyyyMMdd_HHmmss")
            Copy-Item $appCopy $bak -Force
            Write-Host ("  replaced   : " + $n + "  (old copy kept as " + (Split-Path $bak -Leaf) + ")")
        } else {
            Write-Host ("  created    : " + $n)
        }
        Copy-Item $handoff $appCopy -Force
        # the copy keeps the handoff timestamp, which can be OLDER than the object
        # file -> ninja would say "no work to do", so touch it
        (Get-Item $appCopy).LastWriteTime = Get-Date
        $changed++
    }
}

if ($changed -eq 0 -and -not $Force) {
    Write-Host "  nothing to rebuild"
} else {
    # --- 2. rebuild the FSBL ---------------------------------------------------
    $env:PATH = $gccBin + ";" + $env:PATH
    Write-Host "  rebuilding FSBL ..."
    & $ninja -C (Join-Path $fsblDir "build") 2>&1 | Select-Object -Last 4
    if ($LASTEXITCODE -ne 0) { Write-Host "FSBL build FAILED"; exit 3 }

    # --- 3. hand the new elf to the boot component -----------------------------
    Copy-Item $fsblElf $exportElf -Force
    Write-Host ("  updated    : " + (Split-Path $exportElf -Leaf))

    # --- 4. regenerate the boot image -----------------------------------------
    if (-not (Test-Path $bootBin)) {
        Write-Host "  BOOT.bin not found, generating ..."
    } else {
        Copy-Item $bootBin ($bootBin + ".bak") -Force
    }
    & $bootgen -image $bif -arch zynq -o $bootBin -w on 2>&1 | Select-Object -Last 3
    if ($LASTEXITCODE -ne 0) { Write-Host "bootgen FAILED"; exit 4 }
    Write-Host ("  regenerated: " + (Split-Path $bootBin -Leaf))
}

# --- 5. report what is actually inside the images -----------------------------
Write-Host ""
$want = Get-Gem0FromSource (Join-Path $artDir "ps7_init.c")
Write-Host ("  handoff GEM0_CLK_CTRL : " + $want)
$bad = 0
foreach ($img in @($fsblElf, $bootBin)) {
    if (-not (Test-Path $img)) { continue }
    $vals = Get-Gem0Value $img
    $stale = @($vals | Where-Object { $_ -ne $want })
    if ($vals.Count -gt 0 -and $stale.Count -eq 0) {
        Write-Host ("  OK   " + (Split-Path $img -Leaf) + " : " + ($vals -join ", "))
    } else {
        Write-Host ("  FAIL " + (Split-Path $img -Leaf) + " : " + ($vals -join ", "))
        $bad++
    }
}

Write-Host ""
if ($bad -eq 0) {
    Write-Host "RESULT: boot chain is consistent - flash BOOT.bin and test"
    exit 0
}
Write-Host "RESULT: boot chain still inconsistent"
exit 1
