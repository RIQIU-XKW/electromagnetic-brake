# FLASH 固化启动链(以及 2026/10/04 那次"网口连不上"的原因)

## 1. BOOT.bin 里有什么

```
vitis2/app923/flash/untitled.bif
├── [bootloader] soc_923/export/soc_923/sw/boot/fsbl.elf   <- FSBL, 它负责初始化 PS
├──              app923/_ide/bitstream/base_system_9.30.bit <- PL 逻辑
└──              app923/build/app923.elf                    <- 应用
```

QSPI 启动时,**PS 的时钟/PLL/MIO 是由 FSBL 里的 `ps7_init.c` 配置的**;
JTAG 调试时,**同一批寄存器是由 Vitis 用 `app923/_ide/psinit/ps7_init.tcl` 配置的**。

这两份"PS 初始化"必须是同一套值 —— 它们来自同一个硬件交接(`soc_923/hw/sdt/`)。
一旦不一致,同一个 app、同一块板子,走 flash 和走 JTAG 会跑在**不同的时钟树**上。

## 2. 坑:Vitis 不会刷新 FSBL 目录里的 ps7_init

`vitis2/soc_923/zynq_fsbl/CMakeLists.txt` 里是:

```cmake
collect (PROJECT_LIB_SOURCES ps7_init.c)
```

`collect` 取的是 **FSBL 应用目录**里的 `ps7_init.c`。
Vitis 只在"创建平台"时把它拷进去,**平台重新生成(2026/9/30)时不会更新它**。
结果 2026/10/04 的 FSBL 编译的是 **9/23 的旧 PS 配置**(`build.ninja` 里可以确认输入路径):

| 寄存器 | 旧 FSBL(烧进 flash 的) | 当前硬件 / JTAG |
|---|---|---|
| `0xF8000108` IO_PLL_CTRL | FBDIV = 54 | FBDIV = 30 (1000 MHz) |
| `0xF8000138` **GEM0_RCLK_CTRL** | bit4 = 1 | bit4 = 0 |
| `0xF8000140` **GEM0_CLK_CTRL** | `0x00100141` = SRCSEL **EMIO** + DIV0=1 | `0x00100801` = SRCSEL **IO PLL** + DIV0=8 → 125 MHz |
| `0xF8000154` UART_CLK_CTRL | DIV 18 | DIV 10 |
| `0xF800014C` LQSPI_CLK_CTRL | DIV 9 | DIV 5 |
| ... 共 **32** 处不同 | | |

多数时钟只是"IO PLL 1800 MHz + 大分频"和"1000 MHz + 小分频"的等价写法
(UART 都是 100 MHz,所以**串口打印还是正常的**,不看寄存器根本发现不了 ——
`UART_CLK_CTRL` 18/10、`LQSPI_CLK_CTRL` 9/5、`SDIO`、`PCAP`、`FCLK` 全是精确的
1.8 倍,PLL 比例说得很明白)。

真正致命的是 **GEM0**:`bsp/enet/enet_phy.c` 里的 `Enet_Phy_ApplyClock()` 能修
"时钟源被设成 EMIO"和"分频值不对"这两种毛病(`GEM0_CLK_CTRL` 和 `GEM0_RCLK_CTRL`
都管),**但修不了"IO PLL 被配成 1800 而不是 1000"** —— 因为它按 1000 MHz 硬编码:

```c
#define ENET_GEM_CLK_DIV0_IO_PLL    8U      /* 1800/8 = 225 MHz(应为 125 MHz) */
```

于是 PHY 自己能协商、MDIO 也能读(所以日志照样打印 `autonegotiation complete` /
`link speed ...`),可是 GEM 的 RGMII 时钟整体快 1.8 倍 → **一个包都过不去** →
DHCP 拿不到地址 → PC 端连接超时("Connection canceled")。
把 FSBL 换回当前硬件那份后 PLL 回到 1000 MHz,网口立刻正常。

## 3. 修复步骤

```powershell
# 1) 用当前硬件交接覆盖 FSBL 目录里的两份文件
$fsbl = "vitis2\soc_923\zynq_fsbl"
Copy-Item "$fsbl\zynq_fsbl_bsp\hw_artifacts\ps7_init.c" "$fsbl\ps7_init.c" -Force
Copy-Item "$fsbl\zynq_fsbl_bsp\hw_artifacts\ps7_init.h" "$fsbl\ps7_init.h" -Force

# 2) 必须先把时间戳改成现在!复制过去的文件保留了 9/30 的旧时间,
#    而目标 .obj 是 10/4 建的,ninja 会认为 "no work to do" 而跳过重建
(Get-Item "$fsbl\ps7_init.c").LastWriteTime = Get-Date
(Get-Item "$fsbl\ps7_init.h").LastWriteTime = Get-Date

# 3) 重建 FSBL
$env:PATH = 'D:\vivado\Vitis\2024.2\gnu\aarch32\nt\gcc-arm-none-eabi\bin;' + $env:PATH
& 'D:\vivado\Vitis\2024.2\tps\win64\lopper-1.1.0-packages\min_sdk\usr\bin\ninja.exe' -C "$fsbl\build"

# 4) 新 elf 拷到 boot 组件目录, 重新生成 BOOT.bin
Copy-Item "$fsbl\build\fsbl.elf" "vitis2\soc_923\export\soc_923\sw\boot\fsbl.elf" -Force
& 'D:\vivado\Vitis\2024.2\bin\bootgen.bat' -image vitis2\app923\flash\untitled.bif -arch zynq -o vitis2\app923\flash\BOOT.bin -w on
```

**每次改完硬件(重新 Generate Output Products / 导出 XSA)都要走一遍 1~4**,
因为 FSBL 那份 `ps7_init.c` 不会被自动刷新。

## 4. 怎么验证

```powershell
powershell -ExecutionPolicy Bypass -File tools\check_invariants.ps1
```

第 8、9 条检查就是为此加的:

```
  OK    FSBL ps7_init matches the current handoff
  OK    fsbl.elf carries GEM0_CLK_CTRL = 0x00100801
  OK    BOOT.bin carries GEM0_CLK_CTRL = 0x00100801
```

原理:PS 初始化表在二进制里是 `{addr, mask, value, opcode=0x33}` 的小端 32 位字,
直接按 `0xF8000140 / 0x03F03F71 / value` 这三个字找就行。
旧镜像会报 `0x00100141`,一眼就能看出烧的是旧的。

## 5. `Using default Speed from design` 这句话要不要管

`SetUpSLCRDivisors()` 在 SDT 流程下确实被编译成空壳(`#ifndef SDT` / `#else`),
所以日志里必然出现这一行。**但它不影响本工程**:`bsp/enet/enet_phy.c` 里的
`Enet_Phy_ApplyClock()` 会自己按协商速率写 GEM0 的时钟源和分频(`GEM0_CLK_CTRL`
和 `GEM0_RCLK_CTRL` 都管),`踩过的坑` 第 6 条记的就是这件事。

它**修不了**的是"PS 初始化里 IO PLL 本身被配错":

```c
#define ENET_GEM_CLK_DIV0_IO_PLL    8U      /* 按 IO PLL = 1000 MHz 硬编码 */
```

DIV0 = 8 这个常数假定 IO PLL 是 1000 MHz。如果 FSBL 那份 ps7_init 把 IO PLL 配成
别的频率(2026/10/04 那次是 1800 MHz),GEM 时钟就整体偏 1.8 倍 —— **这才是那次
网口不通的真正原因**,和"SDT 空壳"无关。

**所以判断方法很简单**:再遇到"PHY 有链接、MDIO 能读、一个包都过不去",先跑

```powershell
powershell -ExecutionPolicy Bypass -File tools\check_invariants.ps1
```

看 `fsbl.elf` / `BOOT.bin` 里的 PS 初始化是不是当前硬件的那一份,再看 `ps7_init` 里
`IO_PLL_CTRL`(0xF8000108) 的 FBDIV 是不是 30(= 1000 MHz)。
