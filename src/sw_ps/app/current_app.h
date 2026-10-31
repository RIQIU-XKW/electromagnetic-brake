/**
 *******************************************************************************
 * @file    current_app.h
 * @brief   电流指令: 解析上位机帧 + 建电流闭环任务
 *
 * 下行帧(4 字节, 必须同一次 write):
 *     [0xBA][数据低字节][数据高字节][0xAB]      数据 = 目标电流(mA), 0..15000
 * 5 个桥共用同一条电流指令; 数据为 0 = 停(闭环把占空比爬到 0 后关桥输出)。
 *
 * 用 0xBA 而不用 0xAA: 0xAA 已经是"查按键状态"的单字节命令, 抢了会互相打架。
 *******************************************************************************
 */

#ifndef CURRENT_APP_H
#define CURRENT_APP_H

#include <stdint.h>

#include "service/service.h"
#include "service/inter_module/service_net.h"     /* Svc_Net_ReplyTypeDef */

/* -------------------------------------------------------------------------- */
/*  组件参数                                                                   */
/* -------------------------------------------------------------------------- */

#define CURAPP_FRAME_HEAD       0xBAU   /**< 电流指令帧头                      */
#define CURAPP_FRAME_TAIL       0xABU   /**< 电流指令帧尾                      */

#define CURAPP_CURRENT_MAX_MA   15000U  /**< 指令上限(mA) = 15 A               */

/* 闭环任务参数在 service/Algorithm_module/service_current_loop.h 里 */

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                   */
/* -------------------------------------------------------------------------- */

/** @brief  初始化电流闭环组件(5 路 PID)。 */
Svc_StatusTypeDef Current_App_Init(void);

/** @brief  建电流闭环任务(10 ms 一拍)。 */
Svc_StatusTypeDef Current_App_Start(void);

/**
 * @brief  命令处理: 只认 [0xBA][lo][hi][0xAB] 这一种完整帧, 其它一律返回 0 放行。
 * @return 1 = 是本模块处理的, 0 = 不是。
 */
int Current_App_Command(void *Arg, const uint8_t *Data, uint16_t Len,
                        Svc_Net_ReplyTypeDef *R);

#endif /* CURRENT_APP_H */
