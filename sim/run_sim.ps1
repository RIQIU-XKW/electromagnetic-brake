# =============================================================================
# run_sim.ps1 - compile and run the pwm_axi_ip self-checking testbench
# -----------------------------------------------------------------------------
# Uses the Vivado simulator (xvlog / xelab / xsim).  Exits 0 only if every
# check passes, so it can be dropped into a pre-commit script.
#
#   powershell -ExecutionPolicy Bypass -File sim\run_sim.ps1
#   powershell -ExecutionPolicy Bypass -File sim\run_sim.ps1 -VivadoBin D:\Xilinx\Vivado\2024.2\bin
# =============================================================================
param(
    [string]$VivadoBin = "D:\vivado\Vivado\2024.2\bin"
)

$ErrorActionPreference = "Stop"

$simDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$root   = Split-Path -Parent $simDir
$hdlDir = Join-Path $root "ip_repo\pwm_axi_ip_1_0\hdl"

$xvlog = Join-Path $VivadoBin "xvlog.bat"
$xelab = Join-Path $VivadoBin "xelab.bat"
$xsim  = Join-Path $VivadoBin "xsim.bat"

foreach ($tool in @($xvlog, $xelab, $xsim)) {
    if (-not (Test-Path $tool)) {
        Write-Host "ERROR: $tool not found. Pass -VivadoBin <dir>." -ForegroundColor Red
        exit 2
    }
}

Push-Location $simDir
try {
    Remove-Item -Recurse -Force "xsim.dir", "sim_run.log" -ErrorAction SilentlyContinue

    $sources = @(
        (Join-Path $hdlDir "pwm_axi_ip_slave_lite_v1_0_S00_AXI.v"),
        (Join-Path $hdlDir "pwm_half_bridge.v"),
        (Join-Path $hdlDir "pwm_h_bridge.v"),
        (Join-Path $hdlDir "pwm_axi_ip.v"),
        (Join-Path $simDir "tb_pwm_axi_ip.v")
    )
    foreach ($s in $sources) {
        if (-not (Test-Path $s)) { Write-Host "ERROR: missing source $s" -ForegroundColor Red; exit 2 }
    }

    Write-Host "== xvlog ==" -ForegroundColor Cyan
    & $xvlog @sources 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Host "COMPILE FAILED" -ForegroundColor Red; exit 1 }

    Write-Host "== xelab ==" -ForegroundColor Cyan
    & $xelab tb_pwm_axi_ip -s tb_sim 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Host "ELABORATION FAILED" -ForegroundColor Red; exit 1 }

    Write-Host "== xsim ==" -ForegroundColor Cyan
    & $xsim tb_sim -R 2>&1 | Tee-Object -FilePath "sim_run.log" | Out-Host

    $log = Get-Content "sim_run.log" -Raw
    if ($log -match "RESULT: ALL \d+ CHECKS PASSED") {
        Write-Host "SIMULATION PASSED" -ForegroundColor Green
        exit 0
    } else {
        Write-Host "SIMULATION FAILED (see sim\sim_run.log)" -ForegroundColor Red
        exit 1
    }
}
finally {
    Pop-Location
}
