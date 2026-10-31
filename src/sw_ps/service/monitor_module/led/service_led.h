/**
 *******************************************************************************
 * @file    service_led.h
 * @brief   呼吸灯能力(命令队列 + led 任务 + 快照)
 *
 * 点灯是"有命令才做事", 用队列阻塞着等最省 CPU; 调用者只调 Svc_Led_Request*(),
 * 免得出现"谁都在写 LED". LED 状态由本组件统一更新, 别的组件只读快照。
 *
 * 板上 4 路(LED0=K16, LED1=J16, LED2=K14, LED3=J14), 四路共用同一个呼吸频率
 * (硬件只有一组 ramp 计数器), 每颗灯只有"灭"和"按共享频率呼吸"两种状态。
 *
 * 本组件只有能力, 没有命令: 认不认命令、回什么话都在 app 层(inter_app.c + monitor_app.c)。
 *******************************************************************************
 */

#ifndef SERVICE_LED_H
#define SERVICE_LED_H

#include <stdint.h>

#include "service/service.h"
#include "service/inter_module/service_net.h"     /* Svc_Net_CmdTypeDef / Svc_Net_ReplyTypeDef */

/* -------------------------------------------------------------------------- */
/*  组件参数                                                                   */
/* -------------------------------------------------------------------------- */

#define SVC_LED_STACK_WORDS     384U    /**< 1.5 KB                          */
#define SVC_LED_PRIO            2U      /**< 点灯不急, 被抢占也没关系        */

/** 待执行命令队列深度("网口在线"一次压两条, 留点余量)。 */
#define SVC_LED_Q_LEN           8U

/** 板上呼吸灯路数(与 bsp 的 LED_COUNT 一致, 抄一份免得 app 层 include bsp)。 */
#define SVC_LED_COUNT           4U

/** 指示灯(网口在线)对应的那一路。 */
#define SVC_LED_INDEX_INDICATOR 0U

/** 掩码 / 频率的边界(同样是抄 bsp 的值, 应用层不能 include bsp/led/led.h)。 */
#define SVC_LED_MASK_ALL        ((uint8_t)((1U << SVC_LED_COUNT) - 1U))
#define SVC_LED_MASK_OF(Index)  ((uint8_t)(1U << (Index)))
#define SVC_LED_FREQ_MIN        10U     /**< 共享呼吸频率下限(Hz) */
#define SVC_LED_FREQ_MAX        1000U   /**< 共享呼吸频率上限     */

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                     */
/* -------------------------------------------------------------------------- */

/** 常用模式号(数值与 bsp 的 Led_ModeTypeDef 一致)。 */
#define SVC_LED_OFF     0U      /**< 关灯                                        */
#define SVC_LED_SLOW    1U      /**< ~5 s 呼吸                                   */
#define SVC_LED_NORMAL  2U      /**< ~1 s 呼吸                                   */
#define SVC_LED_FAST    3U      /**< ~100 ms 呼吸                                */

/** 一条 LED 命令要做什么。 */
#define SVC_LED_OP_MODE     0U  /**< 按 Mode 点灯(Freq != 0 时按 Freq)            */
#define SVC_LED_OP_FREQ     1U  /**< 只设共享频率, 掩码不变                       */
#define SVC_LED_OP_MASK     2U  /**< 直接设四路掩码                               */
#define SVC_LED_OP_ENABLE   3U  /**< 开关其中一路                                 */

/** 一条 LED 命令。Op 决定用哪几个字段。 */
typedef struct {
    uint8_t  Op;                        /**< SVC_LED_OP_*                      */
    uint8_t  Mode;                      /**< OP_MODE : Led_ModeTypeDef         */
    uint16_t Freq;                      /**< OP_MODE (0=按模式) / OP_FREQ      */
    uint8_t  Index;                     /**< OP_ENABLE: 0..SVC_LED_COUNT-1     */
    uint8_t  On;                        /**< OP_ENABLE: 1=亮 0=灭              */
    uint8_t  Mask;                      /**< OP_MASK  : bit[3:0]                */
} Svc_Led_MsgTypeDef;

/** 给上层看的只读快照。 */
typedef struct {
    uint8_t  Mode;                      /**< 当前模式                         */
    uint8_t  Mask;                      /**< 当前四路使能掩码 bit[3:0]        */
    uint16_t Freq;                      /**< 当前呼吸频率(四路共享)           */
    uint32_t Commands;                  /**< 收到过多少条点灯命令             */
} Svc_Led_SnapshotTypeDef;

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                     */
/* -------------------------------------------------------------------------- */

/** @brief  初始化 LED 驱动 + 命令队列。 */
Svc_StatusTypeDef Svc_Led_Init(void);

/** @brief  初始化好了没有(1 = 好了)。应用层据此决定要不要建任务。 */
uint8_t Svc_Led_IsReady(void);

/** @brief  led 任务入口(队列阻塞着等点灯命令); 任务由应用层创建。 */
void Svc_Led_Task(void *Arg);

/** @brief  请求 LED 状态(Mode = Led_ModeTypeDef, Freq = 0 按模式 / 10..1000 直接设频率)。 */
Svc_StatusTypeDef Svc_Led_Request(uint8_t Mode, uint16_t Freq);

/** @brief  直接设四路使能掩码(bit[3:0] = LED0..LED3, 0 = 全灭)。 */
Svc_StatusTypeDef Svc_Led_RequestMask(uint8_t Mask);

/** @brief  开关其中一路(其它三路不动)。 */
Svc_StatusTypeDef Svc_Led_RequestEnable(uint8_t Index, uint8_t On);

/** @brief  只改共享呼吸频率, 不动掩码(10..1000, IP 会把越界值夹住)。 */
Svc_StatusTypeDef Svc_Led_RequestFreq(uint16_t Freq);

/** @brief  读一份 LED 快照。 */
Svc_StatusTypeDef Svc_Led_GetSnapshot(Svc_Led_SnapshotTypeDef *Out);

/** @brief  模式号 -> 可读名字(off / slow / normal / fast)。 */
const char *Svc_Led_ModeName(uint8_t Mode);

/** @brief  模式号 -> 该模式对应的呼吸频率(Hz)。 */
uint16_t Svc_Led_ModeFreq(uint8_t Mode);

#endif /* SERVICE_LED_H */
