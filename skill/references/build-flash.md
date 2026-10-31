# 构建 / 烧写 / 位流

## 1. 固件（PS 端）

产物：`build/vitis/app923/build/app923.elf`（`board/BOOT.bin` 由另外的流程打包）。

Vitis 工程的 `build/` 目录由 `src/sw_ps/CMakeLists.txt` 生成，**本仓库只保留源码**：
首次构建需在 Vitis 里用 `src/sw_ps` 建 application（或让 Vitis 重新 configure），
输出目录指定到 `<REPO>\build\vitis\app923`。

```powershell
$env:PATH = "D:\vivado\Vitis\2024.2\gnu\aarch32\nt\gcc-arm-none-eabi\bin;" + $env:PATH
& "D:\vivado\Vitis\2024.2\tps\mingw\10.0.0\win64.o\nt\bin\ninja.exe" -C "<REPO>\build\vitis\app923\build"
```

- 必须在 `build\vitis\app923\build` 下跑（否则 `ninja: error: loading 'build.ninja'`）。
- 编译选项含 `-Wall -Wextra -Wundef`，**目标是 0 警告**；`-Wundef` 是为了防"`#if` 里用了没
  include 的宏 → 静默当 0"这类事故。
- 改了源文件清单要同步 `src/sw_ps/UserConfig.cmake`，然后重新 configure
  （`cmake .` 或删 build 目录重来）。

## 2. 上位机（Qt6）

```powershell
$env:PATH = "D:\QT\Tools\mingw1310_64\bin;D:\QT\6.11.2\mingw_64\bin;" + $env:PATH
& "D:\QT\Tools\mingw1310_64\bin\mingw32-make.exe" -j8 -C "<REPO>\build\qt"
```

- 源码在 `src/sw_host/Electromagnetic_braking/`，构建目录建议放在 `build/qt/`（不入库）。

- 界面开着时链接会失败：`ld.exe: cannot open output file ...Permission denied` → 先关界面。
- 改 `.ui` 后 `Automatic MOC and UIC` 会重新生成 `ui_widget.h`；可以用
  `build\...\Electromagnetic_braking_autogen\include\ui_widget.h` 验证新控件有没有进去。

## 3. 位流（PL）

**先搞清楚"哪份工程在综合"** —— 本仓库只保留 `build/vivado_project/` 这一份
（`.xpr` + `.srcs`，**没有 `.runs`/`.gen`**，首次打开需重跑综合）：

| 位置 | 用途 |
|---|---|
| `<REPO>\build\vivado_project\Zynq_Base_Platform.xpr` | 唯一工程源，本仓库的 IP 源码就是它用的 |
| `D:\vivado\project\PS_code\Zynq_Base_Platform` | 原始开发副本（**不在本仓库内**），旧版脚本默认打开的是它 |

改 IP 前先确认要动的 HDL 是被综合的那一份（看 `.gen\sources_1\bd\base_system\ipshared\**\hdl\*.v`
里有没有你的改动），否则会出现"代码改了、板上没变"。注意 `build/vivado_project/` 里不含 `.gen`，
第一次综合后它才会被生成。

重跑一次干净的位流（Vivado 批处理，约十几分钟）：

```powershell
# 只改了 PS 软件 / 只重跑 impl
D:\vivado\Vivado\2024.2\bin\vivado.bat -mode batch -source build\scripts\rebuild_bitstream.tcl

# 改了 IP 的 HDL（src/rtl/**/hdl/*.v）—— 必须带 --full，连 IP 的 OOC 综合一起重跑
D:\vivado\Vivado\2024.2\bin\vivado.bat -mode batch -source build\scripts\rebuild_bitstream.tcl -tclargs --full
```

⚠ 默认只 `reset_run impl_1`，**改了 IP 的 HDL 是不够的**，`--full` 等价于手工跑下面这套：

```tcl
open_project <REPO>/build/vivado_project/Zynq_Base_Platform.xpr
reset_target all [get_ips base_system_pwm_axi_ip_0_0]
generate_target all [get_ips base_system_pwm_axi_ip_0_0]
reset_run base_system_pwm_axi_ip_0_0_synth_1      ;# IP 的 OOC 综合必须重跑
reset_run synth_1
reset_run impl_1
launch_runs base_system_pwm_axi_ip_0_0_synth_1 -jobs 8 ; wait_on_run base_system_pwm_axi_ip_0_0_synth_1
launch_runs synth_1 -jobs 8                            ; wait_on_run synth_1
launch_runs impl_1 -to_step write_bitstream -jobs 8    ; wait_on_run impl_1
```

判断"位流里到底有没有新逻辑"的可靠办法：看这几样产物的时间顺序，并且确认 `.gen` 里的 HDL
含新代码 → IP 的 `*_synth_1\*.dcp` → 顶层 `synth_1\base_system_wrapper.dcp` → `impl_1\*.bit`
→ 导出给 Vitis 的 `build\vitis\soc_923\hw\sdt\base_system_<日期>.bit`。

当前仓库里的位流是 **`base_system_10.28`**（2026-10-28，含"腿 A 静态高 + 腿 B 斩波"的 mode 3 和
EM 低有效修正），三处必须一致：`build/vitis/app923/_ide/bitstream/`、`build/vitis/soc_923/hw/sdt/`、
`board/untitled.bif` 里指的那份。换位流时这三处一起换（后两处是 Vitis 平台生成物，不入库）。

## 4. 烧写

- **JTAG（DDR 调试）**：`build/vitis/app923/_ide/launch.json` 的 `bitstreamFile` 决定 PL 用哪份 bit，
  `elfFile` 是 `build/vitis/app923/build/app923.elf`。改了固件 → 只重下 ELF；改了 PL → 必须 **Program FPGA**，
  否则 FPGA 里还是旧逻辑（现象：模式位写不进去、波形不对）。
- **flash**：`board/untitled.bif` 决定 BOOT.bin 里打哪份 bit，换硬件后要和
  `launch.json` 指到同一个 `.bit`，否则"JTAG 好好的、烧进去就变样"。
- 上电横幅 `[built <日期> <时间>]` 是判断板上跑的是哪一版固件的最快方法。

## 5. 换硬件 / 换 XSA 之后（必做）

```powershell
powershell -ExecutionPolicy Bypass -File build\scripts\refresh_fsbl_psinit.ps1
#   比对 -> 覆盖 FSBL 的 ps7_init.c/.h -> 重建 FSBL -> 重生成 BOOT.bin -> 自检
#   自检项: GEM0_CLK_CTRL 必须是 0x00100801(IO PLL / div8)
```

只检查不动文件：`build\scripts\check_invariants.ps1` 的第 8~10 条。

⚠ FSBL 应用目录里的 `ps7_init.c/.h` 是"没人管的死角"：它们是 Vitis **创建应用那一刻**写进去的，
之后"重新生成平台"只刷新平台/BSP 侧，不会回头更新它。2026-10-04 就因为这份是旧的（IO PLL 被配成
1800 MHz）导致网口"有链接、MDIO 正常、一个包都过不去"。

## 6. 回归测试清单

| 脚本 | 覆盖 |
|---|---|
| `build\scripts\check_invariants.ps1` | 分层不变量、日志锁、RTOS 符号、FSBL/BOOT 的 GEM0（14 项） |
| `sim\host_tests\pwm_logic_test\run.ps1` | PWM 寄存器编码（含 `PWM_EN` 高位保留） |
| `sim\host_tests\led_test\run.ps1` | 4 路掩码/频率/命令（314 项） |
| `sim\host_tests\emmc_test\run.ps1` | CSV 10 列组行、整行装配、条数、废行过滤（289 项） |
| `sim\host_tests\current_loop_test\run.ps1` | 电流闭环 5 路 PID：各桥上限/爬坡/浮点积分/抗饱和/关闭时序/外部停互锁（68 项） |
| ~~`tools\key_test\run.ps1`~~ | **已知失效**（引用已删除的 `service/key/...` 旧路径），**未纳入本仓库**，别当门槛 |

都是"把真实 `.c` 编进 PC 测试主程序 + 假寄存器/假 FatFs/假 ADS"，断言的是代码意图，
硬件那一半仍要实测。`current_loop_test` 用 `setjmp/longjmp` 把 `for(;;)` 控制任务切成单步，
所以能一拍一拍断言。

⚠ `.ps1` 里**只能写 ASCII 注释**：PowerShell 5.1 按 ANSI 读脚本，中文注释的字节可能被解释成
`)`/`"`，把 `@()` 数组字面量提前截断（现象：gcc"少收一个 `-I`"，然后报 `xparameters.h` 找不到）。
另外别在 `.ps1` 里用 `$ErrorActionPreference="Stop"` + 直接调 gcc：gcc 的 warning 走 stderr，
会把脚本中止，看着像编译失败其实测试根本没跑。
