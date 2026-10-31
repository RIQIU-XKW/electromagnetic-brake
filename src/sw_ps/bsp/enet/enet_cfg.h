/**
 *******************************************************************************
 * @file    enet_cfg.h
 * @brief   以太网模块的硬件绑定与编译期配置
 *
 * 本文件全是"编译期事实"或"可调默认值"; 运行期值在 Enet_ConfigTypeDef(enet_types.h),
 * 这样一份固件可以建多个句柄, 也可以不重编就改配置。
 *
 * 硬件(Zynq-7000, GEM0 + RGMII PHY 在 MIO 16..27):
 *   平台 soc_923 / base_system(XPAR_XEMACPS_0_BASEADDR = 0xE000B000)
 *   lwIP lwip220 BSP, NO_SYS = 1, RAW API, 开了 DHCP
 *
 * @note BSP 的 lwIP 是"无操作系统"口(NO_SYS = 1), 所以协议栈只由一个 net 任务驱动,
 *       绝不在中断里跑。
 *******************************************************************************
 */

#ifndef ENET_CFG_H
#define ENET_CFG_H

#include "xparameters.h"

/* -------------------------------------------------------------------------- */
/* Hardware binding                                                           */
/* -------------------------------------------------------------------------- */

/** GEM0 控制器基址(xemac_add() 用)。 */
#define ENET_GEM0_BASEADDR          XPAR_XEMACPS_0_BASEADDR

/* -------------------------------------------------------------------------- */
/* 时间基准(毫秒)                                                              */
/* -------------------------------------------------------------------------- */

/*
 * 时间基准由 OSAL 的 tick 换算提供, 所以下面全用毫秒; 实际精度 = 1/RTOS tick 频率。
 * BSP 里配了 NO_SYS_NO_TIMERS = 1, lwIP 自己不调 sys_check_timeouts(), 必须由应用
 * 按下面的节奏去调它的定时器函数。
 */

/** tcp_fasttmr(): lwIP 硬性要求 250 ms 一次。 */
#define ENET_FAST_TIMER_MS          250U

/** tcp_slowtmr() + dhcp_fine_tmr() + 网络接口维护: 500 ms 一次。 */
#define ENET_SLOW_TIMER_MS          500U

/** dhcp_coarse_tmr(): 60 s 一次。 */
#define ENET_DHCP_COARSE_MS         60000U

/** PHY 链路状态 / 速率轮询: 1 s 一次。 */
#define ENET_LINK_POLL_MS           1000U

/** SI-692601 规避: 100 s 没收到任何帧就复位 GEM0 收发路径, 从"重收包卡死"里救回来。 */
#define ENET_RX_RECOVERY_MS         100000U

/* -------------------------------------------------------------------------- */
/* Interface defaults (seed values for Enet_Config_LoadDefaults())             */
/* -------------------------------------------------------------------------- */

#define ENET_MAC_ADDRESS            { 0x00U, 0x0AU, 0x35U, 0x00U, 0x01U, 0x02U }
#define ENET_IPV4_ADDRESS           { 192U, 168U, 1U, 10U }
#define ENET_IPV4_NETMASK           { 255U, 255U, 255U, 0U }
#define ENET_IPV4_GATEWAY           { 192U, 168U, 1U, 1U }

/** 1 = 启动 DHCP 客户端, 0 = 直接用上面的静态地址。 */
#define ENET_USE_DHCP               1U

/** 租约超时后是否退回静态地址。 */
#define ENET_DHCP_FALLBACK_STATIC   1U

/** DHCP 租约超时, 单位是"500 ms 的慢速服务次数"(24 * 500 ms = 12 s)。 */
#define ENET_DHCP_TIMEOUT_TICKS     24U

/** TCP 服务端口。 */
#define ENET_ECHO_PORT              7U

/** 同时支持的 TCP 通道数(池固定, 不动态分配)。 */
#define ENET_TCP_MAX_CHANNELS       4U

/** 一次 Enet_Poll() 最多从 MAC 队列取几个 pbuf。 */
#define ENET_RX_DRAIN_MAX           16U

/* -------------------------------------------------------------------------- */
/* GEM0 reference clock (Zynq-7000 SLCR)                                      */
/* -------------------------------------------------------------------------- */

/*
 * IO PLL (1000 MHz) / (DIV0 * DIV1) = GEM0 参考时钟
 *   DIV0 = 8, DIV1 = 1  -> 125 MHz (1000 Mbps)
 *   DIV0 = 8, DIV1 = 5  ->  25 MHz (100 Mbps)
 *   DIV0 = 8, DIV1 = 50 -> 2.5 MHz (10 Mbps)
 * DIV1 按协商速率自动选; DIV0 恒定, 取决于 Vivado PS7 配的 IO PLL 频率。
 */
#define ENET_GEM_CLK_DIV0_IO_PLL    8U

#endif /* ENET_CFG_H */
