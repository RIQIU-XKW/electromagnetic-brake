# =============================================================================
# pwm_axi_ip 引脚约束 (Zynq-7020 xc7z020clg400-2)
#
# 端口名必须和 block design 里的外部端口名完全一致。本设计里 BD 端口名带后缀:
#     pwm_out_0[19:0]   (O)   20 路 PWM   (5 个 H 桥 x 4 管)
#     gate_en_0[9:0]    (O)   10 路栅极驱动使能 (5 个 H 桥 x 2 臂)
#     oc_n_1[4:0]       (I)   5 路过流检测, 低有效
# 写成 pwm_out / gate_en / oc_n 会报 "No ports matched" 且引脚不被分配!
#
# 这些约束在 "Generate Output Products" 之前不会生效:
# 只有重新生成 BD 输出产物后, base_system_wrapper.v 里才会出现上述端口。
#
# -----------------------------------------------------------------------------
# 输出索引 -> H 桥 映射 (来自 pwm_axi_ip 端口定义)
#   pwm_out_0[4i+0] = AH (上管, A 臂)      gate_en_0[2i+0] = A 臂使能
#   pwm_out_0[4i+1] = AL (下管, A 臂)      gate_en_0[2i+1] = B 臂使能
#   pwm_out_0[4i+2] = BH (上管, B 臂)      oc_n_1[i]      = 第 i 个桥过流
#   pwm_out_0[4i+3] = BL (下管, B 臂)
#
# 注意: B 臂在 IP 内部是交叉连接的, 所以默认对角驱动下 AH 与 BL 同相。
#       AH/AL 与 BH/BL 之间不存在直通; 同一臂内 AH+AL 由死区保证不重叠。
#
# -----------------------------------------------------------------------------
# 引脚与 BANK 说明 (2026-09-30 更新)
#
#   35 个端口全部落在 bank 13 和 bank 34, 两块都是 3.3V:
#
#     bank 13 (11 个): W6 V6 W8 V8 W9 W10 Y6 Y12 Y13 V10 V11
#       pwm_out_0[0..4] = W10 V10 V8 W9 Y12    桥0 四管 + 桥1 的 AH
#       gate_en_0[0..1] = W6 V6                桥 0 两臂使能
#       oc_n_1[0..1]    = V11 Y13              桥 0 / 桥 1 过流
#
#     bank 34 (24 个): P14 P15 P16 R14 R16 R17 T10 T11 T12 T14 T15 T16
#                      U12 U13 U17 U18 U19 V13 V16 V17 V18 W16 W18 W19
#
#   bank 34 的 VCCO 已确认为 3.3V -> 下面全部用 LVCMOS33。
#   光幕 LS_I[0..5] 也在 bank 34(W15 V12 W13 V15 R19 P19, 见 base_system_pins.xdc),
#   两个文件之间没有任何引脚被用两次。
#
#   这颗 xc7z020clg400-2 上能作 PL IO 的只有 bank 13(25) / bank 34(50) / bank 35(50)。
#   R0~R7 / G0~G7 / B0~B7 要么这颗封装里没有、要么属于 PS 侧的 bank 500/502, 写进
#   约束会让 Vivado 报 "not a valid site or package pin name", 该端口就没有 LOC,
#   最后被 DRC UCIO-1 挡住位流(2026-09-30 踩过: 24 个端口全废)。
#   可用引脚完整清单: tools/package_pins.txt
# =============================================================================

# =============================================================================
# 1. 20 路 PWM 输出  pwm_out_0[19:0]
# =============================================================================

# --- H 桥 0 ---
set_property PACKAGE_PIN W10 [get_ports {pwm_out_0[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[0]}]
set_property PACKAGE_PIN V8 [get_ports {pwm_out_0[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[1]}]
set_property PACKAGE_PIN V10 [get_ports {pwm_out_0[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[2]}]
set_property PACKAGE_PIN W9 [get_ports {pwm_out_0[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[3]}]

# --- H 桥 1 ---
set_property PACKAGE_PIN Y12 [get_ports {pwm_out_0[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[4]}]
set_property PACKAGE_PIN Y6 [get_ports {pwm_out_0[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[5]}]
set_property PACKAGE_PIN R14 [get_ports {pwm_out_0[6]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[6]}]
set_property PACKAGE_PIN V16 [get_ports {pwm_out_0[7]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[7]}]

# --- H 桥 2 ---
set_property PACKAGE_PIN W8 [get_ports {pwm_out_0[8]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[8]}]
set_property PACKAGE_PIN U13 [get_ports {pwm_out_0[9]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[9]}]
set_property PACKAGE_PIN V13 [get_ports {pwm_out_0[10]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[10]}]
set_property PACKAGE_PIN T15 [get_ports {pwm_out_0[11]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[11]}]

# --- H 桥 3 ---
set_property PACKAGE_PIN V17 [get_ports {pwm_out_0[12]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[12]}]
set_property PACKAGE_PIN V18 [get_ports {pwm_out_0[13]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[13]}]
set_property PACKAGE_PIN P14 [get_ports {pwm_out_0[14]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[14]}]
set_property PACKAGE_PIN P16 [get_ports {pwm_out_0[15]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[15]}]

# --- H 桥 4 ---
set_property PACKAGE_PIN W16 [get_ports {pwm_out_0[16]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[16]}]
set_property PACKAGE_PIN W18 [get_ports {pwm_out_0[17]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[17]}]
set_property PACKAGE_PIN W19 [get_ports {pwm_out_0[18]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[18]}]
set_property PACKAGE_PIN U18 [get_ports {pwm_out_0[19]}]
set_property IOSTANDARD LVCMOS33 [get_ports {pwm_out_0[19]}]

# =============================================================================
# 2. 10 路栅极驱动使能  gate_en_0[9:0]   (每桥 2 路: A 臂 / B 臂)
#    ⚠ 这些是驱动器的 EM 脚, **低电平开启输出、高电平关断**(低有效)。
#      IP 内部已取反: PWM_EN 的使能位 = 1 时引脚为低 = 开输出; 复位与过流时引脚为高 = 关断。
#      (与 CTRL.INV_PWM 无关, 那个只翻转 20 路 pwm_out 的电平。)
# =============================================================================

# --- H 桥 0 ---
set_property PACKAGE_PIN W6 [get_ports {gate_en_0[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gate_en_0[0]}]
set_property PACKAGE_PIN V6 [get_ports {gate_en_0[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gate_en_0[1]}]

# --- H 桥 1 ---
set_property PACKAGE_PIN T10 [get_ports {gate_en_0[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gate_en_0[2]}]
set_property PACKAGE_PIN T11 [get_ports {gate_en_0[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gate_en_0[3]}]

# --- H 桥 2 ---
set_property PACKAGE_PIN T12 [get_ports {gate_en_0[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gate_en_0[4]}]
set_property PACKAGE_PIN U12 [get_ports {gate_en_0[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gate_en_0[5]}]

# --- H 桥 3 ---
set_property PACKAGE_PIN T16 [get_ports {gate_en_0[6]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gate_en_0[6]}]
set_property PACKAGE_PIN U17 [get_ports {gate_en_0[7]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gate_en_0[7]}]

# --- H 桥 4 ---
set_property PACKAGE_PIN R16 [get_ports {gate_en_0[8]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gate_en_0[8]}]
set_property PACKAGE_PIN R17 [get_ports {gate_en_0[9]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gate_en_0[9]}]

# =============================================================================
# 3. 5 路过流检测  oc_n_1[4:0]   (输入, 低有效, 上拉)
#
#    低有效 + 上拉: 传感器未接 / 断线时读到的电平是 1 (无过流),
#    不会因为一根线掉了就把整台设备的 PWM 关死, 也不会掩盖真实故障。
#    过流时 IP 内部为组合逻辑直通, 不等时钟, 立即封锁对应 H 桥并锁存,
#    由 CTRL.GLB_EN 0->1 上升沿清除 (STATUS 寄存器可读取 OC_NOW / OC_LATCHED /
#    OC_SRC 定位是哪一路)。
# =============================================================================

set_property PACKAGE_PIN V11 [get_ports {oc_n_1[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {oc_n_1[0]}]
set_property PULLUP true [get_ports {oc_n_1[0]}]

set_property PACKAGE_PIN Y13 [get_ports {oc_n_1[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {oc_n_1[1]}]
set_property PULLUP true [get_ports {oc_n_1[1]}]

set_property PACKAGE_PIN T14 [get_ports {oc_n_1[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {oc_n_1[2]}]
set_property PULLUP true [get_ports {oc_n_1[2]}]

set_property PACKAGE_PIN P15 [get_ports {oc_n_1[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {oc_n_1[3]}]
set_property PULLUP true [get_ports {oc_n_1[3]}]

set_property PACKAGE_PIN U19 [get_ports {oc_n_1[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {oc_n_1[4]}]
set_property PULLUP true [get_ports {oc_n_1[4]}]
