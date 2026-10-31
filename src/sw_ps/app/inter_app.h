/**
 *******************************************************************************
 * @file    inter_app.h
 * @brief   人机交互: STATUS + 按键(0xAA..0xAC 命令 / [0xAA] 上报) + 全板命令地图
 *
 * 按键两个方向:
 *   下行 0xAA 查状态 / 0xAB 软复位(= KEY1) / 0xAC 重启 TCP 服务端(= KEY2)
 *   上行 [0xAA][掩码] 广播, bit0 = KEY1 按着, bit1 = KEY2 按着, 变化才发
 *   (掩码位直接来自 service_key 的事件回调, 见 Inter_KeyAttachReport)
 *
 * 按键任务(20 ms 轮询 + 消抖)自己按就自己动作, 网口这三条只是替代手段 + 查状态。
 *******************************************************************************
 */

#ifndef INTER_APP_H
#define INTER_APP_H

#include <stdint.h>

#include "service/service.h"
#include "service/inter_module/service_net.h"   /* Svc_Net_CmdTypeDef / Svc_Net_ReplyTypeDef */

/* -------------------------------------------------------------------------- */
/*  按键: 下行命令 / 上行上报码                                                 */
/* -------------------------------------------------------------------------- */

#define INTER_KEY_CMD_STATUS    0xAAU   /**< 查按键状态(回一行文本)           */
#define INTER_KEY_CMD_RESET     0xABU   /**< 软复位(= 按 KEY1)                */
#define INTER_KEY_CMD_NET       0xACU   /**< 重启 TCP 服务端(= 按 KEY2)       */

#define INTER_KEY_RPT_STATUS    0xAAU   /**< [0xAA][掩码] 按键状态(变化才发)   */

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief  建本模块的两条任务: net(网络, 必须最先起) + key(按键)。
 * @return SVC_OK / SVC_ERR_STATE(net 组件没就绪) / SVC_ERR_HW(建任务失败)。
 */
Svc_StatusTypeDef Inter_App_Start(void);

/**
 * @brief  把按键的状态变化接到网口上报上(在 Svc_Key_Init 之后调一次)。
 * @return SVC_OK / SVC_ERR_*(Svc_Key_SetEventSink 的返回值)。
 */
Svc_StatusTypeDef Inter_KeyAttachReport(void);

/**
 * @brief  处理 0xAA..0xAC(由命令地图调用)。
 * @return 1 = 报文里有本模块的字节; 0 = 没有(放行)。
 */
int Inter_KeyCommand(void *Arg, const uint8_t *Data, uint16_t Len,
                     Svc_Net_ReplyTypeDef *R);

/**
 * @brief  全板命令地图, 交给 app 注册进 net。
 *
 *   精确名:    STATUS
 *   兜底项:    按键 0xAA..0xAC / 制动 0xA0..0xA5 / 发射 0xD0..0xD6 /
 *              采样 0xE0..0xE2 / 呼吸灯 0xF0..0xFF
 *              (按顺序逐个问, 各自的 handler 返回 0 就把自己摘干净)
 *
 * @note   net 那边只实现"先精确、再兜底"的机制, 不认识任何具体命令。
 */
extern const Svc_Net_CmdTypeDef Inter_App_Commands[];
extern const uint16_t           Inter_App_CommandCount;

#endif /* INTER_APP_H */
