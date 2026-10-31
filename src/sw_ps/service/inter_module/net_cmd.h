/**
 *******************************************************************************
 * @file    net_cmd.h
 * @brief   net 组件内部接口: 命令注册表、分发、文本行比较
 *
 *   net_cmd.c     纯逻辑(注册/查找/整行比较), 不依赖 lwIP, 可在 PC 上单测
 *   service_net.c 任务、队列、enet 句柄、快照
 *******************************************************************************
 */

#ifndef NET_CMD_H
#define NET_CMD_H

#include "service/inter_module/service_net.h"

/** @brief  查一遍注册表: 先精确匹配, 再依次跑兜底表。1 = 有人接手, 0 = 没人认识。 */
int Svc_Net_Cmd_Dispatch(const uint8_t *Data, uint16_t Len, Svc_Net_ReplyTypeDef *R);

/** @brief  清空注册表(测试/复位用)。 */
void Svc_Net_Cmd_Reset(void);

/** @brief  整条报文是否正好等于 Cmd。 */
int Svc_Net_IsCommand(const uint8_t *Data, uint16_t Len, const char *Cmd);

#endif /* NET_CMD_H */
