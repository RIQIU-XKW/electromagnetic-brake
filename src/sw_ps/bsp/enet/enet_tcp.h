/**
 *******************************************************************************
 * @file    enet_tcp.h
 * @brief   TCP 服务层(lwIP RAW API 服务端)
 *
 * 在一个固定的 Enet_TcpChannelTypeDef 池上实现服务端: 每个连接占一个通道,
 * 通道指针经 tcp_arg() 传给 lwIP, 回调一次解引用就拿到状态 —— 不查找、不分配堆。
 *
 * 载荷: 收到的 pbuf 链逐段回显, 受发送窗口(tcp_sndbuf)限制; 应用也可以装接收钩子
 *       (Enet_Tcp_SetReceiveHook)把载荷接走, 于是"回显服务器"变成"命令服务器"
 *       而不用改这个文件。
 *******************************************************************************
 */

#ifndef ENET_TCP_H
#define ENET_TCP_H

#include "enet_types.h"

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/** @brief  在配置的端口上创建、绑定并监听。 */
Enet_StatusTypeDef Enet_Tcp_Start(Enet_HandleTypeDef *h);

/** @brief  关掉所有通道和监听套接字。 */
void Enet_Tcp_Stop(Enet_HandleTypeDef *h);

/** @brief  装应用接收钩子(NULL = 恢复默认回显)。 */
void Enet_Tcp_SetReceiveHook(Enet_HandleTypeDef *h,
                             Enet_TcpReceiveHook hook,
                             void *context);

/** @brief  当前在用的通道数。 */
uint32_t Enet_Tcp_GetActiveChannels(const Enet_HandleTypeDef *h);

/**
 * @brief  在某通道上发数据(先检查发送窗口)。
 * @note   载荷会拷进 lwIP 自己的缓冲, 所以 data 只需在调用期间有效。
 *         返回值: ENET_OK / ENET_ERR_PARAM / ENET_ERR_NO_MEMORY(窗口满) /
 *         ENET_ERR_HW(协议栈拒收)。
 */
Enet_StatusTypeDef Enet_Tcp_Send(Enet_TcpChannelTypeDef *channel,
                                 const void *data, uint16_t len);

/**
 * @brief  把同一段数据发给所有已连接的客户端。
 * @note   给"非网络任务"用: RAW API 只能由一个任务独占, 别的任务把要广播的内容
 *         排进队列, 由网络任务取出来调这个函数。
 * @return ENET_OK(一个客户端都没有也算), ENET_ERR_PARAM, 或 ENET_ERR_HW
 *         (至少有一路因发送窗口满没发出去)。
 */
Enet_StatusTypeDef Enet_Tcp_Broadcast(Enet_HandleTypeDef *h,
                                      const void *data, uint16_t len);

#endif /* ENET_TCP_H */
