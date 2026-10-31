/**
 *******************************************************************************
 * @file    service_ls.h
 * @brief   光幕 6 路采样(周期任务 + 只读快照 + LS 命令)
 *
 *   1. 每 SVC_LS_PERIOD_MS 调 bsp 的 Ls_Scan() 采样 + 消抖;
 *   2. 状态真的变化时打一行日志, 并按 SVC_LS_NOTIFY_TCP 决定要不要广播;
 *   3. 上电先主动报一次当前状态, 免得等第一次遮挡才知道线路通不通;
 *   4. 对外只给只读快照 + 一条 LS 命令。
 *
 * 采样节奏和消抖是组件的固有属性, 命令只读快照, 不碰 XGpioPs 的细节。
 *******************************************************************************
 */

#ifndef SERVICE_LS_H
#define SERVICE_LS_H

#include <stdint.h>

#include "service/service.h"
#include "service/inter_module/service_net.h"     /* Svc_Net_CmdTypeDef / Svc_Net_ReplyTypeDef */

/* -------------------------------------------------------------------------- */
/*  组件参数                                                                   */
/* -------------------------------------------------------------------------- */

#define SVC_LS_PERIOD_MS        5U      /**< 采样周期(ms)                    */
#define SVC_LS_STACK_WORDS      384U    /**< 1.5 KB                          */
#define SVC_LS_PRIO             2U      /**< 5 ms 级抖动无所谓               */

/** 连续多少次同电平才确认状态变化。 */
#define SVC_LS_DEBOUNCE_SCANS   3U

/** 1 = 状态变化时给所有 TCP 客户端广播一行文本。 */
#define SVC_LS_NOTIFY_TCP       1U

/** 1 = 被遮挡时引脚为低(XDC 里有上拉, 光幕输出低有效)。 */
#define SVC_LS_ACTIVE_LOW       1U

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                     */
/* -------------------------------------------------------------------------- */

/** 给上层/命令看的只读快照。 */
typedef struct {
    uint8_t  Raw;                       /**< 最近一次原始电平(1 = 高)      */
    uint8_t  State;                     /**< 消抖后的状态(1 = 该路被遮挡)  */
    uint8_t  Changed;                   /**< 最近一次变化的位              */
    uint32_t Scans;                     /**< 采样次数                      */
    uint32_t Events;                    /**< 状态变化次数                  */
} Svc_Ls_SnapshotTypeDef;

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                     */
/* -------------------------------------------------------------------------- */

/** @brief  初始化光幕驱动(PS EMIO GPIO 6 路输入)。 */
Svc_StatusTypeDef Svc_Ls_Init(void);

/** @brief  初始化好了没有(1 = 好了)。应用层据此决定要不要建任务。 */
uint8_t Svc_Ls_IsReady(void);

/** @brief  光幕采样任务入口(采样 + 消抖 + 变化时上报); 任务由应用层创建。 */
void Svc_Ls_Task(void *Arg);

/** @brief  读一份光幕快照。 */
Svc_StatusTypeDef Svc_Ls_GetSnapshot(Svc_Ls_SnapshotTypeDef *Out);

#endif /* SERVICE_LS_H */
