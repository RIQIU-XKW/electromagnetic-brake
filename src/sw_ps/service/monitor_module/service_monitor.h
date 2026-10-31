/**
 *******************************************************************************
 * @file    service_monitor.h
 * @brief   周期状态日志(每秒一行 + 每 N 行详细状态)
 *
 * 只做两件事:
 *   1. 每秒把 net / pwm 两个组件的只读快照拼成一行日志;
 *   2. 每秒调一次 Svc_Pwm_AlarmPoll(), 每 N 行调一次 Svc_Net_PrintStatus()。
 *
 * STATUS 命令的文本不在这里 —— 那是 app 层的事。
 *******************************************************************************
 */

#ifndef SERVICE_MONITOR_H
#define SERVICE_MONITOR_H

#include <stdint.h>

#include "service/service.h"

/* -------------------------------------------------------------------------- */
/*  组件参数默认值(实际值由 app 通过 Svc_Mon_ConfigTypeDef 传入)               */
/* -------------------------------------------------------------------------- */

#define SVC_MON_PERIOD_MS       1000U   /**< 一行状态日志的周期(ms)          */
#define SVC_MON_FULL_DIV        5U      /**< 每几次打一次完整状态(1=每次)    */
#define SVC_MON_STACK_WORDS     512U    /**< 2 KB                            */
#define SVC_MON_PRIO            1U      /**< 打印最不重要                    */

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                     */
/* -------------------------------------------------------------------------- */

/** monitor 的初始化参数。 */
typedef struct {
    uint32_t PeriodMs;          /**< 状态日志周期(ms), 0 = 用默认             */
    uint32_t FullDiv;           /**< 每几次打一次完整状态, 0 = 不打           */
    int      PwmOcAutoAck;      /**< 1 = 过流恢复后自动确认                   */
} Svc_Mon_ConfigTypeDef;

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                     */
/* -------------------------------------------------------------------------- */

/** @brief  保存监视参数(不建任务, 任务由应用层创建); Cfg 可以为 NULL。 */
Svc_StatusTypeDef Svc_Mon_Init(const Svc_Mon_ConfigTypeDef *Cfg);

/** @brief  monitor 任务入口(一行状态日志 + 过流告警); 任务由应用层创建。 */
void Svc_Mon_Task(void *Arg);

#endif /* SERVICE_MONITOR_H */
