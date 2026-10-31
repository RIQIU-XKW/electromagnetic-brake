# =============================================================================
# base_system 顶层 PL 引脚约束 (Zynq-7000 xc7z020clg400-2)
#
# 本文件管**板级 PL 引脚**, pwm.xdc 管功率级那 35 个脚(bank 13/34)。
#
# 2026/10/13 第二次改板(12:02 顶层重做)后的端口清单:
#   1. LED        led[3:0]                    -> K16/J16/K14/J14   (bank 35)
#   2. RS-485     tx_485 / rx_485 / de_485[0] -> D19/D20/G14        (bank 35)
#   3. 光幕       LS_I[5:0]                   -> W15/V12/W13/V15/R19/P19 (bank 34)
#   4. PS SPI0    SPI0_SCLK / MISO / MOSI     -> N15/N16/M17        (bank 35)
#   5. ADS 控制   gpio_io_o[4:0]              -> M18/L14/L15/L20/M14 (bank 35)
#      输入       gpio2_io_i[0]              -> L19                 (bank 35)
#
# 与上一版的区别(重要, 别照旧图接线):
#   上一版 CS/DAISY/RST 是接在 **PS7 EMIO GPIO 输出**(端口 GPIO_O_0[5:0])上的,
#   和光幕 LS_I 抢同一组 EMIO 位(PS GPIO 54..59)的方向寄存器, 得靠驱动每帧重钉方向。
#   这一版改成了 **一片 AXI GPIO(axi_gpio_1)**:
#       BD 组件 axi_gpio_1, 双通道: 通道1 = 5 位输出(C_GPIO_WIDTH=5) -> gpio_io_o[4:0]
#                                   通道2 = 1 位输入(C_GPIO2_WIDTH=1) -> gpio2_io_i[0]
#   AXI GPIO 的引脚是纯 PL 逻辑, 不再碰 EMIO, 所以和光幕彻底没有冲突了。
#   (GPIO_O_0 这个外部端口在 12:02 的改动里已经删掉, 本文件里的相关约束也一并删了。)
#
# 端口名规则: 必须和 base_system_wrapper.v 里的外部端口名完全一致(区分大小写)。
#   写错或端口已不存在 -> "No ports matched", 该行不生效, 对应输出端口没 LOC ->
#   最后被 DRC UCIO-1 挡住位流(2026/9/30 在这颗芯片上踩过: 24 个端口全废)。
#
# 引脚依据: 与兄弟工程 Zynq_Base_Platform(同一块板、同一颗芯片)已调通的约束对齐;
#   本次 5 个输出 + 1 个输入沿用上一版给 GPIO_O_0 分的那 6 个脚(接线不用动)。
# =============================================================================

# =============================================================================
# 1. LED (breath_led_0/led) —— 4 路, 共享呼吸频率 + 4 路独立使能
# =============================================================================
#   位 0 -> K16 (原来那唯一一颗)   位 1 -> J16   位 2 -> K14   位 3 -> J14
#   都在 bank 35(已证实 3.3V) -> LVCMOS33。
# =============================================================================

set_property PACKAGE_PIN K16 [get_ports {led[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {led[0]}]

set_property PACKAGE_PIN J16 [get_ports {led[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {led[1]}]

set_property PACKAGE_PIN K14 [get_ports {led[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {led[2]}]

set_property PACKAGE_PIN J14 [get_ports {led[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {led[3]}]

# =============================================================================
# 2. RS-485 (axi_uartlite_0 + axi_gpio_0)
# =============================================================================
#   tx_485  (O)  axi_uartlite_0/tx      <-- FPGA 输出, 接收发器 DI
#   rx_485  (I)  axi_uartlite_0/rx      <-- FPGA 输入, 接收发器 RO
#   de_485  (O)  axi_gpio_0/gpio_io_o   <-- 收发器 DE/!RE, 高=发送
#
#   引脚与已验证可通信的参考工程 PL_code/Zynq7020/18_rs485 一致(tx->D19, rx->D20,
#   work_en->G14), **收发不能接反**: D19 接 DI, D20 接 RO; 接反会两个输出对顶。
#   de_485 是普通端口(不是 GPIO 接口端口), 端口名带 [0], 没有 _tri_o 后缀。
# =============================================================================

set_property PACKAGE_PIN D19 [get_ports tx_485]
set_property IOSTANDARD LVCMOS33 [get_ports tx_485]

set_property PACKAGE_PIN D20 [get_ports rx_485]
set_property IOSTANDARD LVCMOS33 [get_ports rx_485]

set_property PACKAGE_PIN G14 [get_ports {de_485[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {de_485[0]}]

# =============================================================================
# 3. 光幕输入 LS_I[5:0] (输入 + 内部上拉, bank 34)
# =============================================================================
#   BD: LS_I[5:0] ---> processing_system7_0/GPIO_I (PS EMIO GPIO 输入, 宽度 6)
#   光幕是干接点/光耦输出, 悬空时电平不定 -> 内部上拉保证"未遮挡"读到确定高电平。
#   (2026/9/30 从 bank 13 的 W6/V6/W8/V8/W9/W10 搬到这里, 那几个脚归 pwm.xdc。)
# =============================================================================

# 位 0 -> W15 (上拉)
set_property PACKAGE_PIN W15 [get_ports {LS_I[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {LS_I[0]}]
set_property PULLUP true [get_ports {LS_I[0]}]

# 位 1 -> V12 (上拉)
set_property PACKAGE_PIN V12 [get_ports {LS_I[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {LS_I[1]}]
set_property PULLUP true [get_ports {LS_I[1]}]

# 位 2 -> W13 (上拉)
set_property PACKAGE_PIN W13 [get_ports {LS_I[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {LS_I[2]}]
set_property PULLUP true [get_ports {LS_I[2]}]

# 位 3 -> V15 (上拉)
set_property PACKAGE_PIN V15 [get_ports {LS_I[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {LS_I[3]}]
set_property PULLUP true [get_ports {LS_I[3]}]

# 位 4 -> R19 (上拉)
set_property PACKAGE_PIN R19 [get_ports {LS_I[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {LS_I[4]}]
set_property PULLUP true [get_ports {LS_I[4]}]

# 位 5 -> P19 (上拉)
set_property PACKAGE_PIN P19 [get_ports {LS_I[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {LS_I[5]}]
set_property PULLUP true [get_ports {LS_I[5]}]

# =============================================================================
# 4. PS SPI0 (EMIO) —— 只有 3 根线
# =============================================================================
#   SPI0_SCLK (O) N15    SPI0_MOSI (O) M17    SPI0_MISO (I) N16
#
#   ⚠ 片选不在 SPI 控制器上: CS/DAISY/RST 是下面第 5 节的 axi_gpio_1 普通输出。
#     原因: 控制器的 SS* 只在**传输期间**被驱动, 空闲时全高, 而 ADS8688 是在 CS
#     下降沿判断菊花链模式的 -> 每个片选周期都被判成"菊花链", SDO 只输出 DAISY 的
#     电平(读回恒 0x00/0xFF), 而 MOSI-MISO 环回测试却一切正常(2026/10/12 踩过)。
#   ⚠ 控制器必须保持**自动片选**(不碰 CR[MANCS]): BD 里 SPI0_SS_I 接的是常量 1'b0,
#     手动片选会让控制器认为自己不是总线主 -> mode fault 且不发时钟。
#   ⚠ MISO 是输入(模块的 SDO 驱动), 悬空时用内部上拉保持确定电平。
# =============================================================================

set_property PACKAGE_PIN N15 [get_ports SPI0_SCLK]
set_property IOSTANDARD LVCMOS33 [get_ports SPI0_SCLK]

set_property PACKAGE_PIN N16 [get_ports SPI0_MISO]
set_property IOSTANDARD LVCMOS33 [get_ports SPI0_MISO]
set_property PULLUP true [get_ports SPI0_MISO]

set_property PACKAGE_PIN M17 [get_ports SPI0_MOSI]
set_property IOSTANDARD LVCMOS33 [get_ports SPI0_MOSI]

# =============================================================================
# 5. ADS8688 控制脚 (axi_gpio_1) —— 2026/10/13 第二次改板新增
# =============================================================================
#   通道 1(5 位输出) -> gpio_io_o[4:0]:
#       gpio_io_o[0] -> M18   CS        片选, 低有效(每帧一个脉冲)
#       gpio_io_o[1] -> L14   DASIY  **必须一直低**(拉高进菊花链模式)
#       gpio_io_o[2] -> L15   RST       低有效复位, 自由态为高
#       gpio_io_o[3] -> L20   备用
#       gpio_io_o[4] -> M14   备用
#   通道 2(1 位输入) -> gpio2_io_i[0]:
#       gpio2_io_i[0] -> L19 备用输入(带上拉, 悬空读到高)
#
#   这 6 个脚全在 bank 35(与 led / RS-485 / SPI0 同一 bank, 已证实 3.3V)
#   -> 统一 LVCMOS33。L20 / M14 / L19 已核对: 这颗封装上是通用 IO, 且本工程
#   两个 xdc 里没有别的端口用它, 无冲突。
#
#   为什么用 AXI GPIO 而不继续用 PS EMIO: EMIO 那 6 位(GPIO_O_0)和光幕 LS_I
#   共用同一个方向寄存器(bank2 DIRM), 光幕驱动把它们配成输入就会把 CS 的输出
#   方向顶掉; AXI GPIO 是纯 PL 逻辑, 两边彻底独立。
#
#   软件侧: 这是普通 AXI GPIO(不是 PS GPIO), 用 XGpio 驱动, 基地址看 xparameters.h
#   的 XPAR_AXI_GPIO_1_BASEADDR; 通道 1 = 输出(GPIO_TRI 对应位清 0),
#   通道 2 = 输入(它只引出了 gpio2_io_i, 通道 1 的 gpio_io_i 在 BD 里接了常量 0)。
# =============================================================================

# CS (gpio_io_o[0]) -> M18
set_property PACKAGE_PIN M18 [get_ports {gpio_io_o[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gpio_io_o[0]}]

# DASIY (gpio_io_o[1]) -> L14
set_property PACKAGE_PIN L14 [get_ports {gpio_io_o[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gpio_io_o[1]}]

# RST (gpio_io_o[2]) -> L15
set_property PACKAGE_PIN L15 [get_ports {gpio_io_o[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gpio_io_o[2]}]

# 备用输出 (gpio_io_o[3]) -> L20
set_property PACKAGE_PIN L20 [get_ports {gpio_io_o[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gpio_io_o[3]}]

# 备用输出 (gpio_io_o[4]) -> M14
set_property PACKAGE_PIN M14 [get_ports {gpio_io_o[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gpio_io_o[4]}]

# 备用输入 (gpio2_io_i[0]) -> L19 (上拉)
set_property PACKAGE_PIN L19 [get_ports {gpio2_io_i[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {gpio2_io_i[0]}]
set_property PULLUP true [get_ports {gpio2_io_i[0]}]

# =============================================================================
# 自检(report_io 里应该看到, 本文件 22 位 + pwm.xdc 35 位 = 57 位):
#   led[0..3]       -> K16 J16 K14 J14           OUT
#   tx_485          -> D19                       OUT
#   rx_485          -> D20                       IN
#   de_485[0]       -> G14                       OUT
#   LS_I[0..5]      -> W15 V12 W13 V15 R19 P19   IN  (PULLUP)
#   SPI0_SCLK       -> N15                       OUT
#   SPI0_MISO       -> N16                       IN  (PULLUP)
#   SPI0_MOSI       -> M17                       OUT
#   gpio_io_o[0..4] -> M18 L14 L15 L20 M14       OUT
#   gpio2_io_i[0]   -> L19                       IN  (PULLUP)
#   pwm_out_0[19:0] / gate_en_0[9:0] / oc_n_1[4:0] 见 pwm.xdc
# =============================================================================
