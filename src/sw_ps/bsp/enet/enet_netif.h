/**
 *******************************************************************************
 * @file    enet_netif.h
 * @brief   lwIP 网络接口服务层
 *
 * 把 lwIP 口 API(lwip_init / xemac_add / xemacif_input)包进模块句柄, 并实现
 * 非阻塞的地址状态机: 静态地址, 或者 DHCP + 限时等待 + 可选回退静态。
 * 本层不阻塞: 超时靠 Enet_Poll() 的 tick 计数, 协商期间照样服务 MAC。
 *******************************************************************************
 */

#ifndef ENET_NETIF_H
#define ENET_NETIF_H

#include "enet_types.h"

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief  初始化 lwIP 并把 MAC 挂到一个 struct netif 上。
 * @note   这里不 up 链路: GEM0 参考时钟要等 xemac_add() 协商出 PHY 速率才能修正,
 *         而修正必须发生在第一次发送之前(见 enet_phy.h)。
 */
Enet_StatusTypeDef Enet_Netif_Attach(Enet_HandleTypeDef *h);

/**
 * @brief  接口 up 并配地址。
 * @note   开了 DHCP 就进入 ENET_STATE_DHCP_WAIT, 地址由 Enet_Netif_Service() 后配;
 *         否则直接用静态地址, 状态变 ENET_STATE_READY。
 */
Enet_StatusTypeDef Enet_Netif_Start(Enet_HandleTypeDef *h);

/** @brief  DHCP 超时状态机推一拍; 等待中返回 ENET_OK, 配不上地址返回 ENET_ERR_DHCP。 */
Enet_StatusTypeDef Enet_Netif_Service(Enet_HandleTypeDef *h);

/** @brief  把 MAC 接收队列排空送进 lwIP; 返回本次处理的帧数。 */
uint32_t Enet_Netif_Input(Enet_HandleTypeDef *h);

/** @brief  打印当前 IPv4 地址 / 掩码 / 网关。 */
void Enet_Netif_PrintAddresses(const Enet_HandleTypeDef *h);

/** @brief  把 IPv4 地址格式化进调用者的缓冲(建议至少 16 字节)。 */
void Enet_Netif_FormatIp(const ip4_addr_t *addr, char *buffer, size_t size);

#endif /* ENET_NETIF_H */
