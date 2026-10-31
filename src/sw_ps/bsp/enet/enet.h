/**
 *******************************************************************************
 * @file    enet.h
 * @brief   以太网模块顶层 API(伞形头文件)
 *
 * 层次:
 *   application       Enet_Init / Enet_Start / Enet_Poll
 *   src/bsp/enet
 *     enet.c          生命周期状态机 + tick 分发
 *     enet_platform.c SCU 定时器 250 ms tick, GIC 连接
 *     enet_phy.c      SLCR GEM0 时钟、链路速率、接收恢复
 *     enet_netif.c    lwIP netif, DHCP / 静态地址状态机
 *     enet_tcp.c      RAW API 服务, 通道池
 *         | xemac_add / xemacif_input / tcp_*      | XScuTimer / XScuGic
 *   lwip220 BSP(NO_SYS = 1)                    Xilinx standalone BSP
 *
 * 用法:
 *   Enet_Config_LoadDefaults(&cfg);
 *   cfg.EchoPort = 7;                        // 改与默认不同的项
 *   Enet_Init(&henet, &cfg);                 // MAC + 时钟 + 中断
 *   Enet_Start(&henet);                      // netif up + TCP 监听
 *   for (;;) { Enet_Poll(&henet); }           // 不阻塞
 *
 * @note Enet_Poll() 是 lwIP 唯一跑的地方, 要调得足够勤(MAC 收包环很小)。
 *******************************************************************************
 */

#ifndef ENET_H
#define ENET_H

#include "enet_cfg.h"
#include "enet_types.h"
#include "enet_platform.h"
#include "enet_phy.h"
#include "enet_netif.h"
#include "enet_tcp.h"

/* -------------------------------------------------------------------------- */
/* Configuration helpers                                                      */
/* -------------------------------------------------------------------------- */

/** @brief  用板级默认值填一份配置(cfg 不能为 NULL)。 */
void Enet_Config_LoadDefaults(Enet_ConfigTypeDef *cfg);

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief  初始化平台、lwIP、MAC 和 GEM0 时钟。
 *
 * 顺序(不能乱):
 *   1. platform 定时器 + tick 计数        (enet_platform.c)
 *   2. lwip_init() 和 xemac_add()        (enet_netif.c)  -> 此时知道 PHY 速率
 *   3. tick 的 GIC 连接                  (enet_platform.c)
 *   4. 按该速率配 GEM0 参考时钟          (enet_phy.c)
 */
Enet_StatusTypeDef Enet_Init(Enet_HandleTypeDef *h, const Enet_ConfigTypeDef *cfg);

/** @brief  接口 up、解析地址、启动服务。 */
Enet_StatusTypeDef Enet_Start(Enet_HandleTypeDef *h);

/** @brief  协议定时器、地址状态机、MAC 输入; 不阻塞, 主循环里持续调。 */
void Enet_Poll(Enet_HandleTypeDef *h);

/* -------------------------------------------------------------------------- */
/* Information                                                                */
/* -------------------------------------------------------------------------- */

/** @brief  把当前链路信息拷进调用者的缓冲(info 不能为 NULL)。 */
Enet_StatusTypeDef Enet_GetLinkInfo(const Enet_HandleTypeDef *h,
                                    Enet_LinkInfoTypeDef *info);

/** @brief  把统计计数拷进调用者的缓冲(stats 不能为 NULL)。 */
Enet_StatusTypeDef Enet_GetStats(const Enet_HandleTypeDef *h,
                                 Enet_StatsTypeDef *stats);

/** @brief  装链路事件钩子(每次链路/速率变化都会调 hook)。 */
void Enet_SetLinkHook(Enet_HandleTypeDef *h, Enet_LinkEventHook hook, void *context);

/** @brief  打印一行状态(状态 / 链路 / 地址 / 计数)。 */
void Enet_PrintStatus(const Enet_HandleTypeDef *h);

/** @brief  生命周期状态的可读名字。 */
const char *Enet_StateName(Enet_StateTypeDef state);

#endif /* ENET_H */
