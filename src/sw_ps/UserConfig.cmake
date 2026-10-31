# Copyright (C) 2023-2024 Advanced Micro Devices, Inc.  All rights reserved.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)

###    USER SETTINGS  START    ###
# Below settings can be customized
# User needs to edit it manually as per their needs.
###    DO NOT ADD OR REMOVE VARIABLES FROM THIS SECTION    ###
# -----------------------------------------
# Add any compiler definitions, they will be added as extra definitions
# Example : Adding VERBOSE=1 will pass -DVERBOSE=1 to the compiler.
#
# SDT: 让 ffconf.h 去读 BSP 生成的 xilffs_config.h(而不是退回 FatFs 默认值)。
#
#   FatFs(xilffs)已经是**平台 BSP 的库**了(见 CMakeLists.txt 的
#   collect(PROJECT_LIB_DEPS ... xilffs ...)), ff.h / ffconf.h / xilffs_config.h
#   都来自平台导出的 include 目录。本工程自己**不再**编译 ff.c / diskio.c。
#
#   为什么还留着 SDT(2026/10/09 实测过, 不是"少了就炸"):
#     它管的是"app 这边看到的 ffconf.h 配置"能不能和 BSP 编库时那份保持一致。
#     ffconf.h 里 FF_MAX_SS / FF_USE_LFN / FF_FS_EXFAT 这些会**影响结构体布局**
#     (ff.h 里 BYTE win[FF_MAX_SS]、LFN 的缓冲区都在 FATFS/FIL 里面)。
#
#     当前 BSP 的配置是 SD 接口 + 不长文件名 + 不 exFAT, 而且 ffconf.h 里
#     FF_MAX_SS 对 SD 路径硬编码成 512(那个 4096 只对 UFS 器件生效) —— 所以
#     **有没有 SDT, sizeof(FATFS)/sizeof(FIL) 都是 576 字节**(实测: 用 nm -S 量
#     数组符号大小, 两种方式都是 0x240)。也就是说今天它是"防御性"的。
#
#     留着它的理由是以后: 一旦有人在 Vitis 里把长文件名/exFAT 打开, 结构体就会变。
#     那时 app 这边若没跟着变, 就是**编得过、链得过、运行时踩内存**。有了 SDT,
#     app 和库永远读同一份 xilffs_config.h, 这种错不可能出现。
set(USER_COMPILE_DEFINITIONS
"SDT"
)

# Undefine any previously specified compiler definitions, either built in or provided with a -D option
# Example : Adding MY_SYMBOL will pass -UMY_SYMBOL to the compiler.
set(USER_UNDEFINED_SYMBOLS
"__clang__"
)


# Add any directories below, they will be added as extra include directories.
# Example 1: Adding /proj/data/include will pass -I/proj/data/include.
# Example 2: Adding ../../common/include will consider the path as relative to this component directory.
# Example 3: Adding ${CMAKE_SOURCE_DIR}/data/include to add data/include from this project.
#
# 本工程把组件自己的 src/ 也加进来, 这样每个文件都能用 "从 src 根算起" 的写法:
#   #include "bsp/pwm/pwm.h"   #include "app/app.h"   #include "service/inter_module/service_net.h"
# 否则只能靠 "相对当前文件" 的写法, 深层目录里的文件就会找不到头文件.
set(USER_INCLUDE_DIRECTORIES
"${CMAKE_SOURCE_DIR}"
)
# 四层结构: osal(系统抽象) / bsp(驱动) / service(业务组件) / app(装配)
# 跨层设施 log(日志) 与 led(呼吸灯) 现在都在 service/monitor_module/ 下面。
#
# ⚠ Mover(直线电机 + 485)是"模块 + 应用"两层(2026-10-16 重构):
#     service/Mover_module   service_motor(帧/CiA402) + service_rs485(总线) + modbus_rtu(组帧)
#     app/Mover_app          0xD0..0xD6 命令 + 发射时序 + 推进任务
#
# ⚠ Samp(采集 + 录卡)同样是两层:
#     service/Samp_module    service_ads(ADS8688 采集 + DATA 广播) + ads8688(器件) +
#                            service_emmc(自动录卡; 命令已删, 见那个文件顶部说明)
#     app/samp_app           0xE0 开始采样 / 0xE1 停止 / 0xE2 查状态
#     app/monitor_app        LED_STA_0..3 + LEDSTAT/LEDMASK/LEDON/LEDOFF/LEDFREQ
#                            (呼吸灯命令; 组件 service/monitor_module/led 只提供能力)
#     app/inter_app          STATUS + KEYxxx, 以及全板统一的命令地图 Inter_App_Commands
#
# ⚠ 制动有两份应用代码(2026-10-30 换方案):
#     app/brake_ctl_app.c    在用: 五路桥同步 + 占空比/死区帧(0xDA../0xDC..) + 光幕只上报
#     app/brake_app.c        保留但不再调用: 旧的"光幕逐路触发、五桥分时投入"时序状态机
#                            (仍参与编译以便继续被类型检查, 只是不建任务、不进命令地图)
set(USER_COMPILE_SOURCES
"main.c"
"app/app.c"
"app/brake_app.c"
"app/brake_ctl_app.c"
"app/Mover_app.c"
"app/samp_app.c"
"app/inter_app.c"
"app/monitor_app.c"
"app/current_app.c"
"osal/osal_freertos.c"
"service/monitor_module/log/log.c"
"service/inter_module/service_net.c"
"service/inter_module/net_cmd.c"
"service/inter_module/net_text.c"
"service/monitor_module/led/service_led.c"
"service/brake_module/service_pwm.c"
"service/brake_module/service_ls.c"
"service/brake_module/service_oc.c"
"service/Mover_module/service_motor.c"
"service/Mover_module/service_rs485.c"
"service/Mover_module/modbus_rtu.c"
"service/monitor_module/service_monitor.c"
"service/Samp_module/ads8688.c"
"service/Samp_module/service_ads.c"
"service/Samp_module/service_emmc.c"
"service/inter_module/service_key.c"
"service/Algorithm_module/service_pid.c"
"service/Algorithm_module/service_current_loop.c"
"bsp/emmc/emmc.c"
"bsp/ps_key/ps_key.c"
"bsp/ps_rst/ps_rst.c"
"bsp/enet/enet.c"
"bsp/enet/enet_platform.c"
"bsp/enet/enet_phy.c"
"bsp/enet/enet_netif.c"
"bsp/enet/enet_tcp.c"
"bsp/led/led.c"
"bsp/ls/ls.c"
"bsp/oc/bsp_oc.c"
"bsp/pwm/pwm.c"
"bsp/ps_spi/ps_spi.c"
"bsp/rs485/rs485.c"
"bsp/rs485/rs485_fifo.c"
"bsp/rs485/rs485_hal.c"
)

# -----------------------------------------

# Turn on all optional warnings (-Wall)
set(USER_COMPILE_WARNINGS_ALL -Wall)

# Enable extra warning flags (-Wextra)
set(USER_COMPILE_WARNINGS_EXTRA -Wextra)

# Warn when an undefined identifier is used in #if / #elif (-Wundef)
#
# ⚠ 这条是 2026-10-16 现场用一整天换来的: 光幕任务创建那段写着
#     #if (APP_ENABLE_LS != 0U) ... #endif
#   而那个文件**没有 include app/app_config.h**, 于是标识符未定义 —— 未定义在 #if 里
#   按 0 处理且 GCC 默认**不报警**, 整段(含"组件没就绪就报错"和"建 LS 任务")被静默
#   编译掉, 函数照样返回成功: 串口显示 "ls/brake ok"、光幕初始化日志也正常, 但光幕
#   任务根本没建 —— "引脚拉低了什么都不动、什么都不打"。
#   开了 -Wundef 之后这种写法会直接变成编译警告, 配合"本工程要求 0 警告"就再也藏不住。
set(USER_COMPILE_WARNINGS_UNDEF -Wundef)

# Make all warnings into hard errors (-Werror)
set(USER_COMPILE_WARNINGS_AS_ERRORS )

# Check the code for syntax errors, but don't do anything beyond that (-fsyntax-only)
set(USER_COMPILE_WARNINGS_CHECK_SYNTAX_ONLY )

# Issue all the mandatory diagnostics listed in the C standard (-pedantic)
set(USER_COMPILE_WARNINGS_PEDANTIC )

# Issue all the mandatory diagnostics, and make all mandatory diagnostics into errors. (-pedantic-errors)
set(USER_COMPILE_WARNINGS_PEDANTIC_AS_ERRORS )

# Suppress all warnings (-w)
set(USER_COMPILE_WARNINGS_INHIBIT_ALL )

# -----------------------------------------

# Optimization level   "-O0" [None], "-O1" [Optimize] , "-O2" [Optimize More], "-O3" [Optimize Most] or "-Os" [Optimize Size]
set(USER_COMPILE_OPTIMIZATION_LEVEL -O0)

# Other flags related to optimization
set(USER_COMPILE_OPTIMIZATION_OTHER_FLAGS )

# -----------------------------------------

# Debug level "" [None], "-g1" [Minimum], "g2" [Default], "g3" [Maximum]
set(USER_COMPILE_DEBUG_LEVEL -g3)

# Other flags related to debugging
set(USER_COMPILE_DEBUG_OTHER_FLAGS )

# -----------------------------------------

# Enable profiling (-pg) (This feature is not supported currently)
# set(USER_COMPILE_PROFILING_ENABLE )

# -----------------------------------------

# Verbose (-v)
set(USER_COMPILE_VERBOSE )

# Support ANSI_PROGRAM (-ansi)
set(USER_COMPILE_ANSI )

# Add any compiler options that are not covered by the above variables, they will be added as extra compiler options
# To enable profiling -pg [ for gprof ]  or -p [ for prof information ]
set(USER_COMPILE_OTHER_FLAGS )

# -----------------------------------------

# Linker options
# Do not use the standard system startup files when linking.
# The standard system libraries are used normally, unless -nostdlib or -nodefaultlibs is used. (-nostartfiles)
set(USER_LINK_NO_START_FILES )

# Do not use the standard system libraries when linking. (-nodefaultlibs)
set(USER_LINK_NO_DEFAULT_LIBS )

# Do not use the standard system startup files or libraries when linking. (-nostdlib)
set(USER_LINK_NO_STDLIB )

# Omit all symbol information. (-s)
set(USER_LINK_OMIT_ALL_SYMBOL_INFO )


# -----------------------------------------

# Add any libraries to be linked below, they will be added as extra libraries.
# User needs to update USER_LINK_DIRECTORIES below with these library search paths.
set(USER_LINK_LIBRARIES
)

# Add any directories to look for the libraries to be linked.
# Example 1: Adding /proj/compression/lib will pass -L/proj/compression/lib to the linker.
# Example 2: Adding ../../common/lib will consider the path as relative to this directory and will pass the path to -L option.
set(USER_LINK_DIRECTORIES
)

# -----------------------------------------

set(USER_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/lscript.ld")

# Add linker options to be passed, they will be added as extra linker options
# Example : Adding -s will pass -s to the linker.
set(USER_LINK_OTHER_FLAGS
)

# -----------------------------------------

###   END OF USER SETTINGS SECTION ###
###   DO NOT EDIT BEYOND THIS LINE ###

set(USER_COMPILE_OPTIONS
    " ${USER_COMPILE_WARNINGS_ALL}"
    " ${USER_COMPILE_WARNINGS_EXTRA}"
    " ${USER_COMPILE_WARNINGS_UNDEF}"
    " ${USER_COMPILE_WARNINGS_AS_ERRORS}"
    " ${USER_COMPILE_WARNINGS_CHECK_SYNTAX_ONLY}"
    " ${USER_COMPILE_WARNINGS_PEDANTIC}"
    " ${USER_COMPILE_WARNINGS_PEDANTIC_AS_ERRORS}"
    " ${USER_COMPILE_WARNINGS_INHIBIT_ALL}"
    " ${USER_COMPILE_OPTIMIZATION_LEVEL}"
    " ${USER_COMPILE_OPTIMIZATION_OTHER_FLAGS}"
    " ${USER_COMPILE_DEBUG_LEVEL}"
    " ${USER_COMPILE_DEBUG_OTHER_FLAGS}"
    " ${USER_COMPILE_VERBOSE}"
    " ${USER_COMPILE_ANSI}"
    " ${USER_COMPILE_OTHER_FLAGS}"
)
foreach(entry ${USER_UNDEFINED_SYMBOLS})
    list(APPEND USER_COMPILE_OPTIONS " -U${entry}")
endforeach()

set(USER_LINK_OPTIONS
    " ${USER_LINKER_NO_START_FILES}"
    " ${USER_LINKER_NO_DEFAULT_LIBS}"
    " ${USER_LINKER_NO_STDLIB}"
    " ${USER_LINKER_OMIT_ALL_SYMBOL_INFO}"
    " ${USER_LINK_OTHER_FLAGS}"
)
