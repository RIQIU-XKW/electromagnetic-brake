# =============================================================================
#  生成 data/pin_assignment.xlsx —— 一张对照表
#
#     原理图页·引脚名称  |  约束文件·端口名  |  实际引脚号  |  排针  |  说明
#
#  数据来源:
#    * 约束文件  pwm.xdc / base_system_pins.xdc   -> 端口名 + 引脚号 + 电平
#    * 接线图    JP1(40 脚) / JP2(52 脚)          -> 原理图信号名 + 排针脚号
#  两者按"实际引脚号"连接起来; 只在图上有、约束里没有的也会列出来。
#
#  用法: powershell -ExecutionPolicy Bypass -File pinmap_gen.ps1
# =============================================================================

$ErrorActionPreference = 'Stop'

# repo root = <script dir>\..\..  (works on any drive / machine)
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$xlsx = Join-Path $repo 'data\pin_assignment.xlsx'
$fPwm = Join-Path $repo 'src\constraints\pwm.xdc'
$fBsp = Join-Path $repo 'src\constraints\base_system_pins.xdc'

function Read-Xdc([string]$file) {
    $pin2port = [ordered]@{}
    $port2io  = @{}
    foreach ($l in (Get-Content -LiteralPath $file)) {
        $t = $l.Trim()
        if ($t -match '^set_property\s+PACKAGE_PIN\s+(\S+)\s+\[get_ports\s+(.+)\]\s*$') {
            $p = $Matches[2].Trim()
            if ($p.StartsWith('{') -and $p.EndsWith('}')) { $p = $p.Substring(1, $p.Length - 2) }
            $pin2port[$Matches[1]] = $p
        }
        if ($t -match '^set_property\s+IOSTANDARD\s+(\S+)\s+\[get_ports\s+(.+)\]\s*$') {
            $p = $Matches[2].Trim()
            if ($p.StartsWith('{') -and $p.EndsWith('}')) { $p = $p.Substring(1, $p.Length - 2) }
            $port2io[$p] = $Matches[1]
        }
    }
    return @{ Pin2Port = $pin2port; Port2Io = $port2io }
}

$P = Read-Xdc $fPwm
$B = Read-Xdc $fBsp

$pin2port = @{}
$port2io  = @{}
$portFile = @{}
foreach ($pair in @(@($P, 'pwm.xdc'), @($B, 'base_system_pins.xdc'))) {
    $src = $pair[0]; $fn = $pair[1]
    foreach ($pin in $src.Pin2Port.Keys) { $pin2port[$pin] = $src.Pin2Port[$pin]; $portFile[$src.Pin2Port[$pin]] = $fn }
    foreach ($port in $src.Port2Io.Keys)  { $port2io[$port] = $src.Port2Io[$port] }
}

# ---- 接线图数据(JP1 40 脚 + JP2 52 脚) -------------------------------------
#  第 1 列 = 全局脚序(1..92), 第 2 列 = FPGA 引脚, 第 3 列 = 原理图信号名, 第 4 列 = 串联电阻
$jpRaw = @(
    @(  1, '',   '5V',                 ''),
    @(  2, '',   'DGND',               ''),
    @(  3, '',   '3.3V',               ''),
    @(  4, '',   'DGND',               ''),
    @(  5, 'D8', 'PWM_EN1 CH1',        'R3 (0R)'),
    @(  6, 'D5', 'PWM_EN2 CH1',        'R4 (0R)'),
    @(  7, '',   '',                   ''),
    @(  8, '',   '',                   ''),
    @(  9, 'W6', 'LLC_PRI PWM1 CH1',   'R7'),
    @( 10, 'V6', 'LLC_PRI PWM3 CH1',   'R8'),
    @( 11, '',   '',                   ''),
    @( 12, '',   '',                   ''),
    @( 13, 'W8', 'LLC_PRI PWM2 CH1',   'R11'),
    @( 14, 'V8', 'LLC_PRI PWM4 CH1',   'R12'),
    @( 15, '',   '',                   ''),
    @( 16, '',   '',                   ''),
    @( 17, 'W9', '',                   'R16'),
    @( 18, 'V10','',                   ''),
    @( 19, '',   'MAX ILD2',           'R15'),
    @( 20, '',   'MAX ILD1',           'R20'),
    @( 21, '',   '',                   ''),
    @( 22, '',   '',                   ''),
    @( 23, 'Y6', 'LLC_PRI PWM2 CH3',   'R19'),
    @( 24, 'Y7', 'LLC_PRI PWM1 CH2',   'R23'),
    @( 25, 'Y8', '',                   ''),
    @( 26, 'Y9', '',                   ''),
    @( 27, '',   'GND(标记剪掉)',      ''),
    @( 28, '',   'GND',                ''),
    @( 29, 'Y11','',                   ''),
    @( 30, 'W11','',                   ''),
    @( 31, '',   'DGND',               ''),
    @( 32, '',   'DGND',               ''),
    @( 33, 'U5', '',                   ''),
    @( 34, 'T5', '',                   ''),
    @( 35, 'V7', '',                   ''),
    @( 36, 'U7', '',                   ''),
    @( 37, 'U8', '',                   ''),
    @( 38, 'U9', '',                   ''),
    @( 39, 'U10','',                   ''),
    @( 40, 'U15','',                   ''),
    @( 41, '',   '5V',                 ''),
    @( 42, '',   'DGND',               ''),
    @( 43, '',   'GND',                'R1'),
    @( 44, '',   'DGND',               ''),
    @( 45, 'R0', '',                   ''),
    @( 46, 'R2', '',                   ''),
    @( 47, '',   'PWM_EN2 CH2',        'R1'),
    @( 48, '',   'PWM_EN1 CH2',        'R2'),
    @( 49, 'R4', '',                   ''),
    @( 50, 'R6', '',                   ''),
    @( 51, '',   'PWM_EN1 CH5',        'R5'),
    @( 52, '',   'PWM_EN2 CH3',        'R10'),
    @( 53, 'R6', '',                   ''),
    @( 54, 'R10','',                   ''),
    @( 55, '',   'LLC_PRI PWM3 CH3',   'R9'),
    @( 56, '',   'LLC_PRI PWM2 CH5',   'R14'),
    @( 57, '',   'MAX ILD3',           'R13'),
    @( 58, '',   'LLC_PRI PWM4 CH4',   'R18'),
    @( 59, '',   'LLC_PRI PWM1 CH4',   'R17'),
    @( 60, '',   'LLC_PRI PWM4 CH3',   'R22'),
    @( 61, 'G0', '',                   ''),
    @( 62, 'G1', '',                   ''),
    @( 63, '',   'PWM_EN1 CH4',        'R21'),
    @( 64, '',   'PWM_EN2 CH4',        'R26'),
    @( 65, 'G2', '',                   ''),
    @( 66, 'G3', '',                   ''),
    @( 67, 'G4', '',                   ''),
    @( 68, 'G5', '',                   ''),
    @( 69, '',   'MAX ILD4',           'R25'),
    @( 70, '',   'DSP MEA_LS1',        'R30'),
    @( 71, '',   'LLC_PRI PWM2 CH4',   'R29'),
    @( 72, '',   'LLC_PRI PWM1 CH5',   'R33'),
    @( 73, 'B0', '',                   ''),
    @( 74, 'B2', '',                   ''),
    @( 75, '',   'LLC_PRI PWM4 CH2',   'R34'),
    @( 76, '',   'LLC_PRI PWM3 CH5',   'R37'),
    @( 77, 'B3', '',                   ''),
    @( 78, 'B4', '',                   ''),
    @( 79, '',   'LLC_PRI PWM3 CH4',   'R41'),
    @( 80, '',   'MAX ILD5',           'R42'),
    @( 81, 'B6', '',                   ''),
    @( 82, '',   'PWM_EN2 CH5',        'R46'),
    @( 83, '',   'DSP MEA_LS1',        'R49'),
    @( 84, '',   'DSP MEA_LS5',        'R50'),
    @( 85, '',   'DSP MEA_LS3',        'R53'),
    @( 86, '',   'DSP MEA_LS4',        'R55'),
    @( 87, 'W13','',                   'R57'),
    @( 88, '',   'DSP MEA_LS5',        'R58'),
    @( 89, 'R19','',                   ''),
    @( 90, '',   'DSP MEA_LS6',        ''),
    @( 91, '',   'DGND',               ''),
    @( 92, '',   'DGND',               '')
)

# 引脚 -> 接线图信息(一个引脚只取第一处)
$schem = @{}
foreach ($x in $jpRaw) {
    if ($x[1] -ne '' -and -not $schem.ContainsKey($x[1])) { $schem[$x[1]] = $x }
}

function Get-JpLabel([int]$idx) {
    if ($idx -le 40) { return ('JP1-' + $idx) }
    return ('JP2-' + ($idx - 40))
}

# ---- 生成主表 ---------------------------------------------------------------
$hdr  = @('原理图页·引脚名称', '约束文件·端口名', '实际引脚号', '排针', '电平', '约束文件', '说明')
$body = @()
$rows = @()

# 1) 约束文件里的每一个端口
foreach ($pin in $pin2port.Keys) {
    $port = $pin2port[$pin]
    $io   = if ($port2io.ContainsKey($port)) { $port2io[$port] } else { '' }
    $sig  = ''; $jp = ''; $note = ''
    if ($schem.ContainsKey($pin)) {
        $s = $schem[$pin]
        $sig = $s[2]
        $jp  = Get-JpLabel ([int]$s[0])
        if ($s[3] -ne '') { $note = '串联电阻 ' + $s[3] }
    }
    if ($sig -eq '') { $sig = '(图上未标)'; $note = '接线图上没有标出这个引脚' }
    # 排序键: 图上有的按排针脚序(00xx), 图上没有的排到后面(1 + 端口名)
    $key = if ($schem.ContainsKey($pin)) { '0' + ([int]$schem[$pin][0]).ToString('0000') } else { '1' + $port }
    $rows += ,@($sig, $port, $pin, $jp, $io, $portFile[$port], $note, $key)
}

# 2) 图上标了引脚、但约束文件里没有的
foreach ($pin in $schem.Keys) {
    if ($pin2port.ContainsKey($pin)) { continue }
    $s = $schem[$pin]
    $note = '**未约束**: 接线图上有引脚, 但两个 xdc 里都没有'
    if ($s[3] -ne '') { $note += ' (串联电阻 ' + $s[3] + ')' }
    $key = '0' + ([int]$s[0]).ToString('0000')
    $rows += ,@($s[2], '(未约束)', $pin, (Get-JpLabel ([int]$s[0])), '', '', $note, $key)
}

# 3) 图上只有信号名、没有引脚号(电源/地/待定)
foreach ($x in $jpRaw) {
    if ($x[1] -eq '' -and $x[2] -ne '') {
        $key = '2' + ([int]$x[0]).ToString('0000')
        $rows += ,@($x[2], '(无 FPGA 引脚)', '', (Get-JpLabel ([int]$x[0])), '', '', '接线图上只标了信号名', $key)
    }
}

# ---- 写 Excel ---------------------------------------------------------------
# 按排序键排列, 再去掉键列
$body = $rows | Sort-Object { $_[7] }
$out  = @(,$hdr)
foreach ($b in $body) { $out += ,@($b[0], $b[1], $b[2], $b[3], $b[4], $b[5], $b[6]) }
$rows = $out

$excel = New-Object -ComObject Excel.Application
$excel.Visible = $false
$excel.DisplayAlerts = $false
try {
    $wb = $excel.Workbooks.Add()
    while ($wb.Worksheets.Count -gt 1) { $wb.Worksheets.Item($wb.Worksheets.Count).Delete() }
    $ws = $wb.Worksheets.Item(1)
    $ws.Name = '引脚分配'
    $r = 1
    foreach ($row in $rows) {
        $c = 1
        foreach ($cell in $row) { $ws.Cells.Item($r, $c).Value2 = [string]$cell; $c++ }
        $r++
    }
    $ws.Rows.Item(1).Font.Bold = $true
    $ws.Rows.Item(1).Interior.Color = 15132390
    $ws.Columns.AutoFit() | Out-Null
    $ws.Application.ActiveWindow.FreezePanes = $false
    $wb.SaveAs($xlsx, 51)
    $wb.Close($false)
    Write-Host ("OK  " + $xlsx)
    Write-Host ("    共 {0} 行" -f ($rows.Count - 1))
} finally {
    $excel.Quit()
    [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($excel)
}
