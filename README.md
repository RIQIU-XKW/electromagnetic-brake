# 磁悬浮式平面电磁制动装置（Zynq-7020）

全国大学生嵌入式芯片与系统设计竞赛 2026（AMD 赛道）作品仓库。

基于 Zynq-7020（Vivado / Vitis 2024.2）的电磁制动试验台：PL 侧自研 **pwm_axi_ip**
（AXI4-Lite 从机，5 个完整 H 桥 = 20 路 PWM / 10 对互补，桥臂死区与桥占空比独立，
5 路过流保护按桥关断，支持线圈 / 涡流制动四种 B 臂模式）；PS 侧 FreeRTOS 固件
（app / service / bsp / osal 四层，11 任务，含电流闭环 PID、EMMC 数据记录、TCP 上报）；
上位机为 Qt6 TCP 客户端（纯 QPainter 实时绘图）。

## 目录结构（按竞赛推荐 3.3.5.4 组织）

```
brake/
├── README.md                  # 本文件：项目简介 + 复现步骤
├── src/                       # 设计源码
│   ├── rtl/                   #   PL 侧 RTL：pwm_axi_ip_1_0（5 路 H 桥 PWM IP）、breath_led_1_0（打包 IP，含 component.xml）
│   ├── constraints/           #   引脚/时钟约束：base_system_pins.xdc、pwm.xdc、pwm_pins.xdc.example
│   ├── sw_ps/                 #   PS 侧固件（C / FreeRTOS，app-service-bsp-osal 四层，Vitis app923 工程 src）
│   ├── sw_host/               #   上位机（Qt6 Widgets，TCP 客户端 + QPainter 绘图）
│   └── driver/                #   pwm_axi_ip C 驱动（pwm_axi_ip_bsp.c/h）
├── sim/                       # 仿真与验证脚本及结果
│   ├── tb_pwm.v / tb_pwm_axi_ip.v / pwm_axi_ip_tb.sv   # PWM IP 仿真激励（177 项检查全过）
│   ├── breath_led_tb.sv       #   呼吸灯 IP BFM 仿真
│   ├── run_sim.ps1 / fix_sim_library.tcl / bd_replica_test.tcl / tb_pwm_behav.wcfg
│   └── host_tests/            #   宿主机回归：pwm_logic_test / led_test / emmc_test / current_loop_test / ls_logic_test
├── build/                     # 可复现的构建脚本 + 工程文件
│   ├── scripts/               #   rebuild_bitstream.tcl、repackage_ip.tcl、add_pwm_ip_to_bd.tcl、
│   │                          #   pinmap_gen.ps1、query_pins.tcl、verify_led_xdc.tcl、
│   │                          #   refresh_fsbl_psinit.*、check_invariants.ps1
│   ├── vivado_project/        #   Zynq_Base_Platform.xpr + .srcs（Block Design + 约束的最小副本）
│   └── xsa/                   #   base_system_10.28.xsa（当前使用的硬件描述，2026-10-28 位流）
├── board/                     # 上板工程与实测输出
│   ├── BOOT.bin / untitled.bif#   烧写文件与打包描述
│   ├── ui_screenshot.ps1      #   上位机界面截图脚本
│   ├── photos/                #   装置实物照片
│   └── outputs/               #   实测输出：GUI 截图、示波器 PWM 波形（pwm_waveform.atkdl）
├── data/                      # 测试数据与参考结果
│   ├── pin_assignment.xlsx    #   引脚分配表
│   └── package_pins.txt       #   器件封装引脚
├── skill/                     # 技能包（AI 协作手册）
│   ├── README.md              #   技能包说明：用法、文件清单、旧→新路径映射
│   ├── SKILL.md               #   一分钟上手、分层硬规则、协议/寄存器摘要、症状→病因速查
│   └── references/            #   architecture / protocol / hardware / build-flash / gui
└── report/                    # 设计报告与文档
    ├── design_report.docx     #   作品报告
    ├── tcp_communication_protocol.docx   # 下位机-上位机 TCP 通信协议
    └── design_notes/          #   flash 启动、SPI 链路设计笔记
```

> 仓库内所有目录名 / 文件名均为纯英文，无中文、空格及特殊字符（中文只出现在文件正文内容中）。
> 少量源码文件保留原始大写命名（如 `Mover_app.c`、`Samp_module/`、`CMakeLists.txt`），
> 因为它们被 `#include` / CMake 直接引用，改名会破坏构建；建议入库时维持原样。

## 复现步骤

环境：Windows + Vivado 2024.2 + Vitis 2024.2 + Qt 6.x（MinGW）。

### 1. PL 位流

```powershell
# 如改动了 IP 的 HDL，先重新打包 IP（新增文件/端口必须同步 component.xml）
D:\vivado\Vivado\2024.2\bin\vivado.bat -mode batch -source build\scripts\repackage_ip.tcl

# 重跑位流（约十几分钟）。只改 PS 软件用默认档；改了 src/rtl 里的 HDL 必须带 --full，
# 否则 IP 的 OOC 综合不重跑，板上行为不会变（详见 skill/references/build-flash.md）
D:\vivado\Vivado\2024.2\bin\vivado.bat -mode batch -source build\scripts\rebuild_bitstream.tcl
D:\vivado\Vivado\2024.2\bin\vivado.bat -mode batch -source build\scripts\rebuild_bitstream.tcl -tclargs --full
```

> `build/vivado_project/` 只保留 `.xpr` + `.srcs`（无 `.runs/.gen`），首次综合会自动生成。

注意：改 IP 的 HDL 后仅 `reset_run impl_1` 不够，需依次 reset 并重跑
IP OOC 综合 → `synth_1` → `impl_1 -to_step write_bitstream`（完整命令见
`skill/references/build-flash.md` 第 3 节）。换位流后三处必须一致：
Vitis `launch.json` 的 bitstreamFile、平台 `hw/sdt/` 下的 .bit、`board/untitled.bif` 指向的 .bit。

### 2. PS 侧固件

```powershell
$env:PATH = "<Vitis>\2024.2\gnu\aarch32\nt\gcc-arm-none-eabi\bin;" + $env:PATH
& "<Vitis>\2024.2\tps\mingw\10.0.0\win64.o\nt\bin\ninja.exe" -C <本仓库路径>\build\vitis\app923\build
```

- build 目录由 Vitis 从 `src/sw_ps/CMakeLists.txt` 生成（建议落在 `build/vitis/app923`），不入库。
- 编译选项 `-Wall -Wextra -Wundef`，目标 0 警告。
- 增删源文件要同步 `src/sw_ps/UserConfig.cmake`（`CMakeLists.txt` / `app.yaml` 是工具生成的）。

### 3. 上位机（Qt6）

```powershell
$env:PATH = "<Qt>\Tools\mingw1310_64\bin;<Qt>\6.x.x\mingw_64\bin;" + $env:PATH
& "<Qt>\Tools\mingw1310_64\bin\mingw32-make.exe" -j8 -C <本仓库路径>\build\qt
```

- 上位机界面运行中时链接会报 `Permission denied`，先关界面再编译。

### 4. 宿主机回归验证（全绿才算改完）

```powershell
powershell -ExecutionPolicy Bypass -File build\scripts\check_invariants.ps1     # 分层不变量 + 启动链
powershell -ExecutionPolicy Bypass -File sim\host_tests\pwm_logic_test\run.ps1  # PWM 寄存器编码
powershell -ExecutionPolicy Bypass -File sim\host_tests\led_test\run.ps1        # 灯（314 项）
powershell -ExecutionPolicy Bypass -File sim\host_tests\emmc_test\run.ps1       # CSV 组行（289 项）
powershell -ExecutionPolicy Bypass -File sim\host_tests\current_loop_test\run.ps1  # 电流闭环 PID（68 项）
```

### 5. 烧写与上板

- JTAG（DDR 调试）：Vitis `launch.json` 指定 bitstreamFile 与 `app923.elf`；
  改固件只重下 ELF，改了 PL 必须 Program FPGA。
- Flash：用 `board/untitled.bif` 打包 `BOOT.bin`（见 `board/BOOT.bin` 为当前版本），
  与 `launch.json` 指向同一份 `.bit`。
- 上电横幅 `[built <日期> <时间>]` 可快速确认板上固件版本。
- 换 XSA 后需重建 platform 并刷新 FSBL/psinit（`build/scripts/refresh_fsbl_psinit.ps1`）。

更多细节（协议与单位、分层规则、踩坑记录）见 `skill/SKILL.md` 与 `skill/references/`。
