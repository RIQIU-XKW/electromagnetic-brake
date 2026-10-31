---
name: electromagnetic-brake-platform
description: Use when working in the brake repo (Zynq-7020 电磁制动试验台: src/sw_ps 固件、src/sw_host Qt 上位机、src/rtl 的 pwm_axi_ip) — covers layering rules, the wire protocol and its units, the PWM power-stage register semantics, build/flash/verify commands, the old→new path map, and the failure→cause table distilled from real bring-up bugs
---

# 电磁制动试验台（Zynq-7020 + FreeRTOS + Qt）

这份 skill 是**做这个工程时的操作手册**：先读“一分钟上手”，需要细节再跳到对应参考文件。
所有命令和常量都在本仓库当前代码里核对过；与代码冲突时以代码为准，并顺手把本文档改对。

## 一分钟上手

```
仓库根 <REPO>（本 skill 位于 <REPO>/skill/，仓库布局见根目录 README.md）
├── src/sw_ps/                     PS 端固件（C，FreeRTOS，四层：app/service/bsp/osal）
├── src/sw_host/Electromagnetic_braking/ 上位机（Qt6 Widgets，TCP 客户端 + 纯 QPainter 绘图）
├── src/rtl/pwm_axi_ip_1_0/        PL 侧 pwm_axi_ip 打包 IP（hdl/ + component.xml，5 H 桥 / 20 路 / 过流）
├── src/rtl/breath_led_1_0/        呼吸灯打包 IP
├── src/constraints/               引脚约束（base_system_pins.xdc / pwm.xdc / pwm_pins.xdc.example）
├── src/driver/                    pwm_axi_ip C 驱动（pwm_axi_ip_bsp.c/h）
├── sim/                           PWM IP testbench + 仿真脚本；host_tests/ 宿主机回归
├── build/scripts/                 位流/重打包/引脚/FSBL 脚本
├── build/vivado_project/          Zynq_Base_Platform.xpr + .srcs（无 .runs/.gen，需重跑综合）
├── build/xsa/                     base_system_10.28.xsa
├── board/                         BOOT.bin / untitled.bif / 实测输出
├── data/                          引脚分配表、封装引脚
└── skill/                         本 skill
```

三步构建（改完代码必做）：

```powershell
# 1) 固件（-Wall -Wextra -Wundef，目标是 0 警告）
#    build 目录由 Vitis 从 src/sw_ps 的 CMakeLists.txt 生成，本仓库只保留源码
$env:PATH = "D:\vivado\Vitis\2024.2\gnu\aarch32\nt\gcc-arm-none-eabi\bin;" + $env:PATH
& "D:\vivado\Vitis\2024.2\tps\mingw\10.0.0\win64.o\nt\bin\ninja.exe" -C "<REPO>\build\vitis\app923\build"

# 2) 上位机
$env:PATH = "D:\QT\Tools\mingw1310_64\bin;D:\QT\6.11.2\mingw_64\bin;" + $env:PATH
& "D:\QT\Tools\mingw1310_64\bin\mingw32-make.exe" -j8 `
  -C "<REPO>\build\qt"     # 源码在 src/sw_host/Electromagnetic_braking，构建目录不入库

# 3) 验证闸门（全绿才算改完）
powershell -ExecutionPolicy Bypass -File build\scripts\check_invariants.ps1        # 分层不变量 + 启动链
powershell -ExecutionPolicy Bypass -File sim\host_tests\pwm_logic_test\run.ps1      # PWM 寄存器编码
powershell -ExecutionPolicy Bypass -File sim\host_tests\led_test\run.ps1            # 灯（314 项）
powershell -ExecutionPolicy Bypass -File sim\host_tests\emmc_test\run.ps1           # CSV 组行（289 项）
powershell -ExecutionPolicy Bypass -File sim\host_tests\current_loop_test\run.ps1   # 电流闭环 PID（68 项）
```

> 旧仓库的 `tools\key_test` 引用删除多年的旧路径，本来就是坏的，**未纳入本仓库**，不要当回归门槛。
> 上位机 exe 正在运行时链接会报 `Permission denied` —— 先关界面再链。
> `.ps1` 一律**只写 ASCII 注释**：PowerShell 5.1 按 ANSI 读脚本，中文注释里被解释成
> `)`/`"` 的字节会把数组字面量提前截断（踩过：gcc 少收了 `-I`，编到真头文件报 `xparameters.h` 找不到）。
> 脚本里也别用 `$ErrorActionPreference="Stop"` + 直接调 gcc：gcc 的 warning 走 stderr，
> 会把整段脚本中止，看起来像"编译失败"，其实测试根本没跑。

## 分层与硬性规则

```
main.c → app/ → service/ → bsp/ → Xilinx
                  ↑
                osal/（唯一允许出现 FreeRTOS 符号的文件：osal_freertos.c）
        log（service/monitor_module/log）是跨层公共设施
```

- `app/` 只管业务：命令认领、时序状态机、装配（初始化顺序 + 建任务 + 命令地图）。**不许 include bsp/**。
- `service/` 只给能力（采样/功率级/光幕/485/网口/灯/按键），**不含命令**；不许 include app/。
- `bsp/` 全部硬件接口，不许 include app/ 或 service/。
- **任务一律在 app 层创建**（`Inter_App_Start` / `Monitor_App_Start` / `BrakeCtl_App_Start` /
  `Mover_App_Start` / `Samp_App_Start` / `Current_App_Start`），service 只导出 `Svc_Xxx_Task()`。
- 十一个任务：net(2 ms,prio4) key(20 ms,2) led(队列,2) mon(1 s,1) ls(5 ms,2) brkctl(5 ms,3)
  rs485(起端口后睡,3) mover(时序等待,2) ads(10 ms,3) emmc(队列+1 s 刷盘,2) curloop(10 ms,3)。
- 加文件要同步 `src/sw_ps/UserConfig.cmake`；`CMakeLists.txt` / `app.yaml` 是工具生成的，
  长期改动别只放这里。

## 协议与单位（详见 references/protocol.md）

- 下发：文本命令只有 `PING`、`STATUS`（整行比较）；其余全是**单字节命令**，在整条报文里扫描命中。
- 单字节段：`0xA0..0xA5` 制动、`0xD0..0xD6` 发射小车、`0xE0..0xE2` 采样、`0xF0..0xFF` 呼吸灯、
  `0xAA..0xAC` 按键。
- 制动参数用**帧**：`[0xDA][hi][lo][0xDB]` 占空比 0..70%；`[0xDC][hi][lo][0xDD]` 死区 0 或 100..10000 ns。
- 电流指令用**帧**：`[0xBA][lo][hi][0xAB]` 电流 0..15000 mA —— ⚠ **低字节在前**（和上面两条相反），
  5 个桥共用同一个目标。帧头不能用 `0xAA`（按键命令/上报已占用）。
- 上报：文本回话（只回发起者）、`DATA` 行（10 ms 一行，广播）、`[0xB0]/[0xC0]/[0xAA]` 二进制广播。
- `DATA` 行现在 **10 个数**（只许在末尾加）：
  `DATA <秒> <i1..i5 A> <动子速度 mm/s> <母线电压 V> <动子位移 mm> <激光引脚电压 V>`。
  第 7/9 个是 2026-11 新加的动子速度/位移，第 10 个是排障用的原始激光电压。
- 单位：线圈电流 `I[A] = (1.5·V − 2.5)·10`（= `I[mA] = 15·(V[mV] − 零点)`，零点默认 1667 mV，
  现场用 `0xE3` 在制动关闭时标定）；母线 = CH5 引脚电压 × 11。

## 激光测距（动子位移/速度，详见 references/hardware.md）

传感器 ST-M2 小型点激光，**模拟输出 4~20 mA**，板上 **100 Ω** 取样电阻变成电压进 ADS8688 的 CH6：

- 阈值就是手册 4~20 mA 档的余量/错误值 ×100 Ω：`3.0 mA→300 mV`、`3.8→380`、`20.2→2020`、`21.0→2100`。
  `<300`/`>2100` = LOST（读数无效）；`380..2020` = TRACK；中间两段只保持状态。切换需连续 3 拍（防抖）。
- **两点标定**（现场实测）：`0.732 V → 0 mm`、`1.143 V → 480 mm`，
  `x[mm] = x1 + (x2−x1)/(y2−y1)·(V−y1)`，即 **1 mV ≈ 1.17 mm**。四个数在 `service_ads.h`。
- 速度只在"上一拍+当前拍都 TRACK"时算 `|Δx|/Δt`（Δt = 10 ms），状态跳变清位置缓存；丢失报 0。
- 平滑链：位移一阶低通(0.35) → 差分 → 速度一阶低通(0.30) → **死区 20 mm/s**（静止必须读 0，因为取绝对值会把噪声整成正的）。
- ⚠ 分辨率上限就在标定上：411 mV 跨 480 mm，1 LSB ≈ 0.09 mm，10 ms 差分有 ±9 mm/s 量化噪声。
  想更细就收窄传感器的映射起点/终点，或改读它的 RS485 数字量。

## 两条控制链路（占空比模式 vs 电流闭环）

| | 占空比模式 | 电流闭环 |
|---|---|---|
| 命令 | `0xA1` + `0xDA`/`0xDC` 帧 | `0xBA` 电流帧 |
| 模块 | `app/brake_ctl_app.c`（brkctl，5 ms） | `service/Algorithm_module/`（curloop，10 ms） |
| 输出 | 五桥同一个占空比 | 5 个独立 PID，各桥上限 37/40/28/45/75 % |

**规矩是"最后一条命令说了算"**，实现见 `Svc_CurLoop_Stop()` 的三处调用：

- 电流指令 > 0 → 闭环接管：自己开全局调制 + 开五桥，并把五桥死区设成 **100 ns**（只设一次）。
- `0xA5` / 过流 / 收到占空比帧 → 闭环**立刻放手**。⚠ 这是安全项：闭环只要目标 > 0 就会自己开桥，
  不按住它，"点了停止制动却关不掉"。
- 放手后闭环不再写占空比（按"上次写下去的值"去重），不会把占空比模式的值抹成 0；
  但退坡到 0 的那一拍必须写一次 0，否则残余占空比留在寄存器里。
- 上位机 PWM 栏的 `0xDC` 死区帧任何时候都有效，闭环不会每拍刷回 100 ns。
- PID：积分纯 float、条件积分抗饱和、每拍占空比增量 ≤ 2 %；参数都是首版估值，现场按响应调。

## 硬件与功率级要点（详见 references/hardware.md）

- `PWM_EN[9:0]` = 10 个半桥的 gate 使能，**`PWM_EN[19:10]` = 每桥 2 位的腿 B 模式**：
  `0` 对角驱动 / `1` 腿 B 静态低 / `2` 腿 B 静态高 / **`3` 腿 A 静态高 + 腿 B 斩波（本项目制动用）**。
- 制动的电气形态：左桥 Q1(AH) 常开、Q3(AL) 常关；右桥 Q2(BH)/Q4(BL) 带死区互补斩波；
  占空比 = Q4 导通占比，负载平均电压 = 占空比 × Vbus，单向电流、慢衰减。
- 使能脚 `gate_en`（EM）**低有效**：引脚低 = 开输出、高 = 关断。`PWM_EN` 的使能位仍是
  "1 = 使能"，IP 内部取反后输出，所以复位/过流时引脚为高（关断）。
- 引脚以 `src/constraints/pwm.xdc` + `data/pin_assignment.xlsx` 为准
  （同桥内 Q1=PWM1、Q2=PWM2、Q3=PWM3、Q4=PWM4；表里 C 列是球号、D 列是丝印，别混）。

## 症状 → 病因速查（本次 bring-up 真实踩过）

| 症状 | 病因 | 修法 |
|---|---|---|
| JTAG 下载后能打印但**网口连不上**，串口有 `App_Init() failed, halted` | 换掉旧 `Svc_Brake_Init()` 后漏调 `Svc_Pwm_Init()/CheckId()`，`App_Init` 直接失败 → `main` 死循环 → **任务一个都没建** | 在 `BrakeCtl_App_Init()` 最前面补 `Svc_Pwm_Init()` + `Svc_Pwm_CheckId()` |
| **启动制动后又变回对角驱动**，拖一次占空比才恢复 | `Pwm_SetHalfBridgeEnable/SetBridgeEnable/SetAllHalfBridges/Pwm_Stop` 里用 `&= 0x3FF` 或直接写 `0x3FF/0` 覆盖整个 `PWM_EN`，把 `[19:10]` 的模式位清零 | 只改 `[9:0]`，其余位原样写回；`0xA1` 里**先使能、最后写模式和占空比** |
| 同一次点击里 `0xA0/0xA1` **没生效**，只有占空比/死区变了 | 处理函数"认出一条就 `return 1`"，同一条 TCP 报文里后面的命令被丢弃 | 整条报文按顺序扫完（帧 4 字节步进、单字节 1 字节步进），最后返回"是否处理过" |
| 改了 IP 的 HDL 但板上**还是旧行为** | ① 综合用的是**另一份**工程副本；② 只 `reset_run impl_1` 没重跑 IP 的 OOC 综合；③ flash 启动用的是 `untitled.bif` 里的旧 `.bit` | 确认改动进了被综合的那份 `.gen` HDL，重跑 IP synth + 顶层 synth + impl；`bif` / `launch.json` 指向新 bit |
| 光幕置 0 不识别、时序不推进 | `#if (APP_ENABLE_LS != 0U)` 而该文件没 include `app_config.h` → 未定义标识符当 0 → 整段（含 `Osal_TaskCreate`）被悄悄编译掉，函数还返回成功 | 用 `Svc_Ls_IsReady()` 判断；编译选项加 `-Wundef`（已加） |
| flash 启动网口"有链接、一个包都不过" | FSBL 应用目录的 `ps7_init.c` 不会随"重新生成平台"更新，GEM0 时钟被配成 1.8 倍 | 改硬件后跑 `build\scripts\refresh_fsbl_psinit.ps1`，它比对/覆盖/重建/自检 GEM0=`0x00100801` |
| 位移/速度曲线**数值整体偏大或偏小**（形状对） | 标定 `y1/y2` 猜错（曾按"0.4 V + 1.6 V 跨度"算，实际两点是 0.732→1.143 V 跨 480 mm，差 3.9 倍） | 推一次全程，读 `laser_mv` 起止值填进 `SVC_ADS_LASER_Y1_V/Y2_V`，`X2_MM` 填实际行程 |
| 动子**停住**了速度还一直有读数 | 位移只剩 ±1 LSB(0.09 mm) 抖动，10 ms 差分 ≈ 十几 mm/s，而速度取了绝对值 → 噪声被整流成永远为正 | 速度死区 `SVC_ADS_LASER_SPEED_DEADBAND_MMS`（默认 20 mm/s）；想看方向把 `SPEED_ABS` 改 0 |
| 速度曲线中途**又冒一个峰** | 速度是 `|Δx/Δt|`，位移只要回升（机械回弹/光斑跳变/丢目标后重捕获）就会出现第二个峰 | 看 CSV 的 `laser_mv`：电压也回升 = 机械/光斑；跳到 ~300/2100 mV = 丢过目标 |
| 点了"关闭制动"却**关不掉** | 电流闭环每 10 ms 一拍，只要目标 > 0 就自己开桥，没人按住它 | `Svc_CurLoop_Stop()` 必须出现在 `BrkCtl_AllOff`、过流分支、0xA1、占空比帧四处 |
| `.ps1` 里明明写了 `-I` 却报 `xparameters.h` 找不到 | `.ps1` 里写了中文注释，PowerShell 按 ANSI 读脚本时字节被解释成 `)`，把 `@()` 数组提前截断 | `.ps1` 只写 ASCII 注释（`current_loop_test/run.ps1` 是范例） |

## 不要做的事

- **别用 PowerShell 的 `Get-Content`/`Set-Content` 改源码**：会把 UTF-8 中文再解释一遍变 mojibake。
  用编辑器/`write`/`edit` 工具，改完 grep 一下中文还在不在。
- 别手改工具生成物：`app923/_ide/psinit/ps7_init.tcl`、`app.yaml`、`CMakeLists.txt`、
  BSP `hw_artifacts/*`；要改就改硬件/配置再生成。
- 别在中断里做协议解析；别在 net 任务以外调 lwIP；别绕开 OSAL 直接调 FreeRTOS API。
- 别把 `.gitignore` 里的可再生产物（Vivado `.cache/.gen/.runs`、GUI `build/`、仿真波形）塞进仓库 ——
  除非用户明确要求"全部推送"。

## 参考文件

- `README.md` — 技能包说明：用法、文件清单、**旧路径 → 新路径映射表**
- `references/build-flash.md` — 构建/烧写/位流重综/工程副本的坑
- `references/protocol.md` — 完整命令表、帧格式、上报格式、单位换算
- `references/architecture.md` — 目录地图、十一个任务、命令地图、关键 API
- `references/hardware.md` — 引脚表、PWM IP 寄存器与模式、过流、ADS8688 + 激光测距、eMMC、按键
- `references/gui.md` — 上位机结构、左右双图与铺满、制动/电流页控件与限值

> 文中出现的旧路径（`vitis2/app923/src`、`qt/Electromagnetic_braking`、`ip_manage/pwm_ip`、
> `Zynq_Base_Platform.*`、`tools/`）已全部按竞赛推荐目录改写；剩余提及仅用于对照历史记录，
> 映射关系见 `README.md` 的路径映射表。
