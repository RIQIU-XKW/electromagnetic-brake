/**
 *******************************************************************************
 * @file    service_oc.h
 * @brief   过流(OC)业务服务 —— 5 路过流状态 + 变化事件(给 brake_app 用)
 *
 * 层次: app -> 本组件 -> bsp/oc -> PL PWM IP 的 OC 状态位。
 * 本层加的是业务语义: "现在是不是告警""哪几路新报的"(事件位, 取走即清)、
 * "本次上电以来报过哪几路"(位图), 以及一拍一次的采样。
 *
 * 不做软件滤波: 硬件已两级同步 + 锁存, 再加延时只会推迟保护动作。
 * 不做保护动作、不发送报文(那都是 app 的事)。
 *******************************************************************************
 */

#ifndef SERVICE_OC_H
#define SERVICE_OC_H

#include <stdint.h>

#include "service/service.h"

/** 路数 = H 桥个数(= PL PWM IP 的 oc_n 位数)。 */
#define SVC_OC_CHANNELS         5U

/** 位图掩码: bit i = 第 i 路 / 第 i 个桥。 */
#define SVC_OC_MASK_ALL         ((1U << SVC_OC_CHANNELS) - 1U)

/** 给上层看的只读快照。 */
typedef struct {
    uint8_t  Ready;             /**< 1 = 已绑定 PWM 句柄                       */
    uint8_t  Alarm;             /**< 1 = 此刻处于过流告警(有输入在拉低)        */
    uint32_t NowMask;           /**< 此刻报过流的桥                            */
    uint32_t LatchedMask;       /**< 硬件粘滞位: 报过过流的桥                  */
    uint32_t EventMask;         /**< 本次上电以来出现过的桥(软件累计)          */
    uint32_t Polls;             /**< 采样次数                                  */
    uint32_t RawStatus;         /**< 原始 STATUS 寄存器(排障)                  */
} Svc_Oc_SnapshotTypeDef;

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                     */
/* -------------------------------------------------------------------------- */

/** @brief  把 bsp/oc 绑到 PWM 句柄上(必须在 Svc_Pwm_Init() 之后调)。 */
Svc_StatusTypeDef Svc_Oc_Init(void);

/** @brief  采一拍: 读硬件状态 -> 更新"此刻/累计"位图 -> 把新出现的位放进事件位图。 */
void Svc_Oc_Poll(void);

/** @brief  此刻是否处于过流告警(输入正在拉低, 或者硬件锁存着一次过流事件)。 */
int Svc_Oc_IsAlarm(void);

/** @brief  此刻报过流的桥(位图; 只剩锁存时给的是锁存里的分桥位)。 */
uint32_t Svc_Oc_GetNowMask(void);

/** @brief  硬件粘滞位(报过过流的桥, 直到 acknowledge)。 */
uint32_t Svc_Oc_GetLatchedMask(void);

/** @brief  本次上电以来出现过的桥(位图, 软件累计, 只有 Acknowledge 才清)。 */
uint32_t Svc_Oc_GetEventMask(void);

/** @brief  有没有过流发生过(含粘滞与软件累计)。 */
int Svc_Oc_AnyFault(void);

/**
 * @brief  清过流状态: 清硬件粘滞位 + 清软件累计/事件位图。
 * @note   会顺手把 PWM 全局使能关一下再开, 所以只在"确认功率级安全"之后调。
 */
Svc_StatusTypeDef Svc_Oc_Acknowledge(void);

/** @brief  读一份快照。 */
Svc_StatusTypeDef Svc_Oc_GetSnapshot(Svc_Oc_SnapshotTypeDef *Out);

#endif /* SERVICE_OC_H */
