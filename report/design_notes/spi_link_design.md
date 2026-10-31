# ZYNQ ↔ STM32 SPI 链路设计备忘

> 状态:**已实现**(2026-09-29)。PC 侧回归全绿:`tools/proto_crosscheck` 现在自动发现
> 两个 STM32 工程(实验29 RS-485 / 实验66 SPI)各跑一遍,共 18 + 21 项全过。
> 上板联调按第七节的顺序做。
> 目标:Zynq(PS SPI0 via EMIO)做**主机**,STM32H743(SPI2)做**从机**,跑现有 RS-485 帧协议。

## 零、实现落点(先看这张表)

| 位置 | 文件 | 说明 |
|---|---|---|
| Zynq 驱动(bsp) | `vitis2/app923/src/bsp/ps_spi/ps_spi.{h,c}` | XSpiPs 主机:Mode 0 / 参考时钟 166.4 MHz 分频到 ≈1.3 MHz / 片选 SS0 / 40 字节定长事务 |
| Zynq 任务(app) | `vitis2/app923/src/app/app_spi.c` | 每 20 ms 一次事务;收到的字节喂 `AppProto_ParseByte` |
| Zynq 业务(app) | `app.c` 的 `App_HandleRemoteFrame()` | **两条链路共用**的命令处理(点灯/转发/回状态) |
| Zynq 配置 | `app/app_config.h` | `APP_SPI_PERIOD_MS=20`、`APP_SPI_SILENT_MS=3000`、`APP_Q_SPI_LEN=4` |
| STM32 驱动 | `实验66 ZYNQ SPI从机通信/Drivers/BSP/SPI_SLAVE/spi_slave.{h,c}` | SPI2 从机 + 中断常驻武装(`HAL_SPI_TransmitReceive_IT`) |
| STM32 链路层 | 同工程 `Drivers/BSP/LINK/zynq_link.{h,c}` | 与实验29 **同一份代码**,只把传输换成 SPI(3 处改动) |
| STM32 主程序 | 同工程 `User/main.c` | 按键上行 + LCD 显示 + 每秒一帧从机心跳 |
| MDK 工程 | 同工程 `Projects/MDK-ARM/atk_h743.uvprojx` | target 名 `SPI_SLAVE`;已加入 spi_slave.c / zynq_link.c,移除 NRF24L01 |
| 回归测试 | `tools/proto_crosscheck/run.ps1` + `spi_slave_stub.c` | SPI 侧也用同一份 `test_cross.c` 验证 |

## 一、已确认的三项决定

| 项 | 决定 |
|---|---|
| 接线 | Zynq 的 4 根信号线接到 STM32 的 **SPI2**:SCLK→**PB13**、MISO→**PB14**、MOSI→**PB15**、CS→**PB12** |
| 协议 | **沿用现有 RS-485 帧**:`0xAA 0x55 LEN CMD PAYLOAD CRC8`(poly 0x07/init 0x00),命令号、LED 模式值、STATUS 布局全部与 `app/app_proto.c` / STM32 `zynq_link.c` 一致 |
| 握手 | **只用 4 线**(SCLK/MISO/MOSI/CS),主机**定时轮询**;`L14(DAISY)`、`L15(RST)` 不使用 |

## 二、引脚对照(Zynq 侧 XDC 已就位)

| Zynq 端口 | 封装脚 | 方向 | STM32 脚 | STM32 功能 |
|---|---|---|---|---|
| `SPI0_SCLK` | N15 | O | PB13 | SPI2_SCK |
| `SPI0_MISO` | N16 | I | PB14 | SPI2_MISO |
| `SPI0_MOSI` | M17 | O | PB15 | SPI2_MOSI |
| `SPI0_CS`   | M18 | O | PB12 | SPI2_NSS |
| `SPI0_DAISY`| L14 | O | — | **实为 SS2**(第 3 个片选),本链路不用,保持高 |
| `SPI0_RST`  | L15 | O | — | **实为 SS1**(第 2 个片选),本链路不用,保持高 |

⚠️ 硬件上这 6 个脚都在 **bank 35(3.3V)**,和 STM32 的 3.3V IO 电平匹配 ✔;两端必须**共地**。

⚠️ BD 里那两个脚叫 `RST`/`DAISY`,**名字有误导**:它们是 SPI 控制器 CR[SSCTRL] 位上
第 2、3 个片选输出,不是复位/菊花链信号。`ps_spi.c` 初始化时把 CR[SSCTRL] 写成 `0xE`
(只有 SS0 有效),所以它们在传输期间也保持高电平。

⚠️ XDC 里曾经为 8 个"备用 SPI 接口端口"(`SPI_0_0_*`)加过占位 LOC(为满足 `UCIO-1`),
**现在 BD 里已经没有这些端口了**,那段约束已注释掉;`report_io` 里应只看到上面 6 个脚。

## 三、SPI 时序与速率(两侧必须完全一致)

| 参数 | 取值 | 说明 |
|---|---|---|
| 模式 | **CPOL=0, CPHA=0(Mode 0)** | 空闲低、上升沿采样。Zynq 侧只设 `XSPIPS_MASTER_OPTION`(不设 `CLK_ACTIVE_LOW` / `CLK_PHASE_1`);STM32 侧 `SPI_POLARITY_LOW` + `SPI_PHASE_1EDGE` |
| 位序 | MSB first | 两侧都是默认 |
| 数据宽度 | 8 bit | |
| 时钟 | 主机出,`SCLK = 参考时钟 / 2^(N+1)`,参考 166.4 MHz、N=6 → **≈1.3 MHz** | 由 `PsSpi_PrescalerFor()` 自动挑最接近 `PS_SPI_TARGET_HZ`(默认 1 MHz)的值 |
| CS | 硬件片选:主机用 PS SPI0 的 SS0(经 EMIO 到 `SPI0_CS`),从机用 **NSS 硬件输入**(PB12) | 从机必须 `SPI_NSS_HARD_INPUT`;软件 NSS 会让从机无法感知帧边界 |

> 注:实验37 原来的 SPI2 是**主机 + Mode 3**(CPOL=1/CPHA=1)+ 软件 NSS。新工程没有沿用,
> 而是新建了 `SPI_SLAVE/spi_slave.c`(从机 + Mode 0 + 硬件 NSS),原来的 `SPI/spi.c` 已从工程移除。

## 四、数据交换模型(定长事务,最简可靠)

SPI 是全双工:主机每个节拍同时收/发。所以约定**固定长度报文**:

```
每次事务 = 40 字节(PS_SPI_FRAME_LEN / SPI_SLAVE_FRAME_LEN, 两边必须一致)
  主机 → 从机: 40 字节, 前面是待发帧(没有就全 0), 其余补 0
  从机 → 主机: 40 字节, 同理(从机必须在 CS 拉低之前把待发帧填进 TX 缓冲)
```

* 两侧用**同一个字节流解析器**(`AppProto_ParseByte` / `zynq_link.c` 的状态机),
  帧靠 `0xAA 0x55` 同步,尾部补 0 不会误判 ✔
* **轮询周期**:主机每 **20 ms**(`APP_SPI_PERIOD_MS`)发一次事务;没有待发帧时全 0
* **从机不能主动说话**:它只能在"下一帧"里回答,所以一次往返最迟 20 ms。
  实际实现里 STM32 **每秒主动发一帧 `0x14 SLAVE_STATUS`**(按键次数/收帧数/错误数),
  Zynq 据此判断从机在线(超过 3 s 收不到任何帧就告警)

命令集合(见 `app/app_proto.h`,STM32 见 `zynq_link.h`):

| CMD | 方向 | 载荷 | 含义 |
|---|---|---|---|
| 0x01 | STM32→ZYNQ | `[mode]` | LED 模式(按键触发) |
| 0x02 | STM32→ZYNQ | `[freq_lo freq_hi]` | 呼吸频率 |
| 0x10 | 双向 | `[data…]` | 透传数据(TCP ↔ STM32) |
| 0x11 | ZYNQ→STM32 | 8 字节状态 | LED/网口/光幕状态(收到命令后的应答) |
| 0x12 | STM32→ZYNQ | — | 心跳(每 2 s),Zynq 收到回 0x11 |
| 0x13 | ZYNQ→STM32 | `[state changed events]` | 光幕状态变化(仅 RS-485 上主动发) |
| 0x14 | STM32→ZYNQ | `[keys rx_lo err_lo]` | 从机在线心跳(每 1 s);Zynq 只记账,不回帧 |

## 五、Zynq 侧实现(app923)

1. **`bsp/ps_spi/ps_spi.h/.c`** —— 用 `XSpiPs` 驱动 PS SPI0:
   * `PsSpi_Init(h, PS_SPI_DEV_BASEADDR)`:`XSpiPs_LookupConfig` → `CfgInitialize` →
     `XSpiPs_SelfTest` → `SetOptions(XSPIPS_MASTER_OPTION)` → `SetClkPrescaler(N)` →
     `SetSlaveSelect(0)`(把 CR[SSCTRL] 写成 0xE,只有 SS0 有效)→ 就绪
   * `PsSpi_ExchangeFrame(h)`:收发 `h->TxBuf` / `h->RxBuf` 一整帧(`XSpiPs_PolledTransfer`)
   * `PsSpi_LoopbackTest(h)`:回环自测(需 MOSI-MISO 短接)
   * `PsSpi_GetCtrlReg()` / `PsSpi_PrintStatus()` / 统计计数
   * **注意**:PS SPI0 走 EMIO,`XPAR_XSPIPS_0_*` 已在 BSP 里 ✔ 不需要重建平台
2. **`app/app_spi.c`** —— 独立任务(优先级与 rs485 同级,栈 2 KB):
   每 20 ms 从 `QSpiTx` 取一帧装进 TxBuf(没有就清零)→ 一次事务 → 逐字节喂解析器 →
   `App_HandleRemoteFrame()`,要回状态就排进 `QSpiTx` 由下一帧带走
3. **TCP 测试命令**(`app_net.c` 的 `App_Net_HandleSpi`):
   `SPIID` / `SPISTAT` / `SPIPING`(立刻做一次事务)/ `SPILOOP` / `SPISEND <文本>`
4. **`tools/proto_crosscheck`**:`spi_slave_stub.c` 模拟"每事务 40 字节、不足补 0",
   与 Zynq 端 `app_proto.c` 交叉验证(含 0x14)

## 六、STM32 侧实现(基于 实验37)

1. **复制**实验37 为新工程 `D:\stm32_hal_code\H743IIT6\example_project\实验66 ZYNQ SPI从机通信`
   (**原实验37 未改动**,可随时对照;`spi.c` / `nrf24l01.c` / `qspi.c` 已从新工程移除)
   * 命名说明:原计划的"实验38"已被现有的 `实验38 FLASH模拟EEPROM实验` 占用,故用 66
2. **新增 `Drivers/BSP/SPI_SLAVE/spi_slave.{h,c}`**(从机驱动,不动原来的 `SPI/spi.c`)
   * `spi_slave_init()`:`SPI_MODE_SLAVE` + `SPI_NSS_HARD_INPUT` + `SPI_POLARITY_LOW` +
     `SPI_PHASE_1EDGE` + 8BIT + MSB;PB12/13/14/15 复用 AF5;使能 `SPI2_IRQn`
   * 常驻武装:`HAL_SPI_TransmitReceive_IT(tx, rx, 40)`,完成回调里立即再次武装;
     出错回调 `HAL_SPI_Abort()` 后再武装(主机复位会把 HAL 卡在忙状态)
   * 对外只有两个动作:`spi_slave_send(frame, len)` / `spi_slave_receive(buf, max)`
3. **`Drivers/BSP/LINK/zynq_link.{h,c}`**:从实验29 复制,**只改传输层 3 处**
   (`#include`、`rs485_send_data` → `spi_slave_send`、`rs485_receive_data` → `spi_slave_receive`),
   并新增 `zlink_send_slave_status()`;帧格式/CRC/命令号与实验29 完全一致
4. **`User/main.c`**:LCD 显示发送命令与 Zynq 状态;KEY0/1/2/WKUP 切 LED 模式;
   每秒发一帧从机心跳,每 2 s 发一帧 PING;LCD 上的 "SPI Txn" 每 20 ms 加 1
5. **`User/stm32h7xx_it.c`**:新增 `SPI2_IRQHandler → HAL_SPI_IRQHandler(&g_spi_slave_handle)`
   (少了它就一个字节也收不到)
6. 本工程**不含 RS-485**(SPI 与 RS-485 是两块板之间的两种接法;Zynq 侧两条链路可共存)

## 七、验证顺序(上板)

1. Zynq 单独:TCP 敲 `SPIID`(看 sclk/cs/cr)与 `SPISTAT`(txn 每 20 ms 加 1)
2. 环回验证(先不接 STM32):把 Zynq 的 MOSI 与 MISO 短接 → `SPILOOP` 应回 PASS,
   证明控制器与引脚 OK
3. 接上 STM32:**共地**,4 线对接;拔掉板上 NRF24L01 模块(U5,它和 PB13/14/15 共用)
4. `SPISTAT` → "stm32 N ms ago" 应稳定在几百毫秒内;按 STM32 的键 → `slave keys` 增长
5. STM32 按键 → Zynq 的 PL 灯 K16 变化,STM32 LCD 显示 Zynq 状态
6. TCP 敲 `SPISEND hello` → STM32 LCD 的 "Net Data" 出现 hello
7. 用示波器看 SCLK/MOSI/MISO/CS(Mode 0、CS 低有效、40 字节一次事务)

## 八、待办

已完成:

- [x] Zynq:`bsp/ps_spi/`(XSpiPs 主机驱动)+ 编译 0 error / 0 warning
- [x] Zynq:`app_spi.c` 任务 + `app_proto` 复用 + TCP 测试命令(`SPIID/SPISTAT/SPIPING/SPILOOP/SPISEND`)
- [x] Zynq:`app_proto.h` 增加 `0x14 SLAVE_STATUS`,`app.c` 两条链路共用帧处理
- [x] STM32:复制 实验37 → `实验66 ZYNQ SPI从机通信`,SPI2 改从机(Mode 0 + 硬件 NSS)
- [x] STM32:`zynq_link.c` 换传输层 + `spi_slave.c` 定长事务 + LCD 显示 + 按键上行
- [x] STM32:MDK 命令行编译 0 Error / 0 Warning(target `SPI_SLAVE`)
- [x] 两侧协议一致性回归(`tools/proto_crosscheck` 现在同时验证 485 与 SPI 两个工程)

待做(需要硬件):

- [ ] 上板联调(第七节 1~7 步)
- [ ] 视调试情况调整速率(改 `PS_SPI_TARGET_HZ`)与周期(`APP_SPI_PERIOD_MS`)
- [ ] 如果不需要 RS-485 与 SPI 同时接,可在 `app_config.h` 里关掉其中一条链路以省资源
