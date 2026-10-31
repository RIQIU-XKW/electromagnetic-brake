# 技能包：electromagnetic-brake-platform

面向 **Zynq-7020 磁悬浮式平面电磁制动装置** 的 AI 协作技能包（Agent Skill）。
作用是让 AI 助手（或新加入的队员）在动手改代码之前，先拿到这个项目**已验证过的事实**：
分层规则、寄存器语义、通信协议与单位、构建/烧写流程，以及一份从真实 bring-up 事故里
抽出来的「症状 → 病因 → 修法」速查表。

## 一、怎么用

**给 AI 助手（Claude / Cursor / WorkBuddy 等支持 Agent Skill 的环境）**

把 `skill/` 目录放进技能目录（或直接指定路径），然后在对话里说：

> 按 brake 项目的 skill 包规范改电流闭环：把爬坡限制从 2 %/拍 改成 1.5 %/拍，改完跑一遍验证闸门

助手会自动读到 `SKILL.md` 的「一分钟上手」和「分层与硬性规则」，并按 `references/` 里的细节执行。
也可以显式点名：

> 读 `skill/SKILL.md`，按里面的「不要做的事」检查我刚改的 `service_pwm.c`

**给人（新队员接手）**

1. 先读仓库根 `README.md` —— 项目是什么、怎么跑起来；
2. 再读 `skill/SKILL.md` —— 代码怎么组织、哪些坑已经踩过；
3. 需要细节时按下面「文件清单」跳进 `references/`。

## 二、文件清单

| 文件 | 内容 | 什么时候看 |
|---|---|---|
| `SKILL.md` | 一分钟上手（目录树 + 三步构建 + 验证闸门）、分层硬规则、协议与单位摘要、两条控制链路对比、功率级要点、症状→病因速查、不要做的事 | **每次动手前** |
| `references/architecture.md` | 固件目录地图（app/service/bsp/osal 每个文件干什么）、11 个任务与周期/优先级、命令地图、关键 API | 找代码位置、加新模块 |
| `references/protocol.md` | 完整命令表（单字节段 0xA0..、0xBA 电流帧、0xDA/0xDC 制动帧）、`DATA` 行 10 列含义、单位换算 | 改上位机/固件通信 |
| `references/hardware.md` | 引脚表（PWM/EN/过流/光幕/按键/eMMC）、`pwm_axi_ip` 寄存器与 4 种 B 臂模式、ADS8688 + 激光测距标定、eMMC、按键 | 动 RTL、改引脚、调采样 |
| `references/build-flash.md` | 固件/上位机/位流三条构建链、IP 重打包与 OOC 重综合、JTAG 与 flash 烧写、换 XSA 后必做项、回归测试清单 | 出位流、上板、换硬件 |
| `references/gui.md` | 上位机结构（widget / plotwidget / ledindicator）、TCP 发送约定（裸字节 vs 文本）、双图布局与控件限值 | 改 Qt 界面 |

## 三、路径映射（旧仓库 → 本仓库）

本技能包原先按开发仓库布局编写，整合进竞赛仓库后已按推荐目录改写；下表用于对照历史记录、
issue 描述或旧截图里的路径。

| 旧路径 | 本仓库路径 | 说明 |
|---|---|---|
| `vitis2/app923/src/` | `src/sw_ps/` | PS 侧固件源码（四层结构不变） |
| `vitis2/app923/build/` | `build/vitis/app923/build/` | Vitis 构建产物，不入库 |
| `vitis2/app923/flash/` | `board/` | `BOOT.bin` / `untitled.bif` |
| `vitis2/*.xsa` | `build/xsa/base_system_10.28.xsa` | 只保留当前使用的硬件描述 |
| `vitis2/引脚分配.xlsx` | `data/pin_assignment.xlsx` | 引脚分配表 |
| `vitis2/PWM波形.atkdl` | `board/outputs/pwm_waveform.atkdl` | 实测波形 |
| `qt/Electromagnetic_braking/` | `src/sw_host/Electromagnetic_braking/` | Qt6 上位机 |
| `ip_manage/pwm_ip/ip_repo/pwm_axi_ip_1_0/` | `src/rtl/pwm_axi_ip_1_0/` | 打包 IP（hdl + component.xml） |
| `ip_manage/breath_led_ip/ip_repo/breath_led_1_0/` | `src/rtl/breath_led_1_0/` | 呼吸灯 IP |
| `ip_manage/pwm_ip/driver_example/` | `src/driver/` | `pwm_axi_ip_bsp.c/h` |
| `ip_manage/pwm_ip/sim/` | `sim/` | testbench + 仿真脚本 |
| `ip_manage/pwm_ip/scripts/repackage_ip.tcl` | `build/scripts/repackage_ip.tcl` | IP 重打包 |
| `Zynq_Base_Platform.xpr` / `.srcs/` | `build/vivado_project/` | 仅 .xpr + .srcs，无 .runs/.gen |
| `tools/*_test/` | `sim/host_tests/` | 宿主机回归（去掉 .exe） |
| `tools/*.tcl`、`tools/*.ps1` | `build/scripts/` | 位流/引脚/FSBL 脚本 |
| `docs/` | `report/design_notes/` | flash 启动、SPI 链路笔记 |
| `skill/` | `skill/` | 本技能包 |

## 四、脚本的可移植性改动

整合时对几个脚本做了「去绝对路径」处理，换机器/换盘符不用改脚本：

- `build/scripts/rebuild_bitstream.tcl` — 按脚本位置定位 `.xpr`；新增 `-tclargs --full`
  一次跑完 IP OOC 重综合 + `synth_1` + `impl_1`（改 IP 的 HDL 时必须带，否则板上行为不变）。
- `build/scripts/verify_led_xdc.tcl` — 按脚本位置定位工程与 XDC；注释全部改为 ASCII
  （Vivado 在 Windows 下 `-source` 含非 ASCII 的 tcl 可能报 `[Common 17-180] Spawn failed`）。
- `build/scripts/pinmap_gen.ps1` — 引脚表输出改到 `data/pin_assignment.xlsx`，输入改到
  `src/constraints/*.xdc`，仓库根由 `$PSScriptRoot` 推导。
- `build/scripts/query_pins.tcl` — 封装引脚表输出改到 `data/package_pins.txt`。
- `board/ui_screenshot.ps1` — 默认 exe 指向 `build/qt/`，截图存 `board/outputs/`。

## 五、维护约定

- 改了协议/寄存器/引脚，**同一次提交里同步改 `references/`**，别让技能包落后于代码；
- 新的 bring-up 事故解决后，往 `SKILL.md` 的「症状 → 病因」表加一行（症状、病因、修法各一句）；
- `.ps1` / `.tcl` / `.bat` 只写 ASCII 注释（PowerShell 5.1 按 ANSI 读，Vivado 控制台是 GBK 代码页）；
- 与代码冲突时**以代码为准**，并顺手把文档改对。
