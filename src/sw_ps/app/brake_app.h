/**
 *******************************************************************************
 * @file    brake_app.h
 * @brief   制动业务: 单字节命令 0xA0..0xA5 + 制动时序状态机 + 上报 0xB0/0xC0
 *
 * 下行(上位机 -> 板子, TCP 上直接发一个字节):
 *   0xA0  使能(打开 PWM 全局调制)        0xA2/0xA3/0xA4  强度 低30%/中60%/高90%
 *   0xA1  启动时序(开桥1, 等光幕1)        0xA5  关闭制动(停时序 + 全关 + 清过流)
 *
 * 上行(板子 -> 上位机, 2 字节定长, 广播):
 *   [0xB0][掩码]  光幕状态: bit i = 第 i+1 路被遮挡
 *   [0xC0][掩码]  过流告警: bit i = 第 i+1 个桥过流
 *
 * 时序(由 brake 任务每 5 ms 推一拍, 光幕 n 拉低 = 车到第 n 路):
 *   0xA1       -> 桥1 预置 10%
 *   光幕1 拉低 -> 桥1 升到档位最大值, 桥2 预置
 *   光幕2 拉低 -> 桥2 升满, 桥3 预置, **桥1 关**
 *   光幕3/4/5  -> 同理接力(桥 n 升满、桥 n+1 预置、桥 n-1 关)
 *   光幕6 拉低 -> 关桥5, 时序结束
 *   过流       -> 立刻关全部桥, 进 fault, 要重新 0xA0 + 0xA1 才能恢复
 *******************************************************************************
 */

#ifndef BRAKE_APP_H
#define BRAKE_APP_H

#include <stdint.h>

#include "service/service.h"
#include "service/inter_module/service_net.h"     /* Svc_Net_CmdTypeDef / Svc_Net_ReplyTypeDef */

/* -------------------------------------------------------------------------- */
/*  命令码 / 上报码                                                            */
/* -------------------------------------------------------------------------- */

#define BRAKE_CMD_ENABLE        0xA0U   /**< 制动使能(全局调制开)             */
#define BRAKE_CMD_START         0xA1U   /**< 启动时序                         */
#define BRAKE_CMD_LEVEL_LOW     0xA2U   /**< 强度: 低(30%)                    */
#define BRAKE_CMD_LEVEL_MID     0xA3U   /**< 强度: 中(60%)                    */
#define BRAKE_CMD_LEVEL_HIGH    0xA4U   /**< 强度: 高(90%)                    */
#define BRAKE_CMD_STOP          0xA5U   /**< 关闭制动(停时序 + 全关 + 清过流) */

#define BRAKE_RPT_LS            0xB0U   /**< [0xB0][光幕遮挡掩码]              */
#define BRAKE_RPT_OC            0xC0U   /**< [0xC0][过流桥掩码]                */

/** 过流告警的保持窗口(ms): 报出去的告警至少保持这么久, 期间引脚恢复也不报"已恢复"。 */
#define BRAKE_OC_HOLD_MS        2000U

/** 保持窗口折算成任务拍数。 */
#define BRAKE_OC_HOLD_TICKS     ((BRAKE_OC_HOLD_MS) / (BRAKE_TASK_PERIOD_MS))

/* -------------------------------------------------------------------------- */
/*  占空比(%)                                                                  */
/* -------------------------------------------------------------------------- */

#define BRAKE_DUTY_LOW_PERCENT  30U
#define BRAKE_DUTY_MID_PERCENT  60U
#define BRAKE_DUTY_HIGH_PERCENT 90U

/** 桥刚打开、还没等到自己那一路光幕时的"预置"占空比(所有档位共用)。 */
#define BRAKE_DUTY_PRESTART_PERCENT 10U

/* -------------------------------------------------------------------------- */
/*  规模与任务参数                                                             */
/* -------------------------------------------------------------------------- */

#define BRAKE_BRIDGE_COUNT      5U      /**< H 桥个数                         */
#define BRAKE_LS_COUNT          6U      /**< 光幕路数(LS_I[0..5])             */
#define BRAKE_TASK_PERIOD_MS    5U      /**< 周期任务节拍                     */
#define BRAKE_TASK_STACK_WORDS  512U
#define BRAKE_TASK_PRIO         3U      /**< 和 ads 同级, 保护动作要跟得上     */

/* -------------------------------------------------------------------------- */
/*  状态(内部变量)                                                                 */
/* -------------------------------------------------------------------------- */

typedef enum {
    BRAKE_ST_IDLE = 0,      /**< 未使能                                  */
    BRAKE_ST_ENABLED,       /**< 已 0xA0, 等 0xA1                        */
    BRAKE_ST_WAIT_LS1,      /**< 桥1 预置已开, 等光幕1                   */
    BRAKE_ST_SEG1,          /**< 已到光幕1, 等光幕2                      */
    BRAKE_ST_SEG2,          /**< 已到光幕2, 等光幕3                      */
    BRAKE_ST_SEG3,          /**< 已到光幕3, 等光幕4                      */
    BRAKE_ST_SEG4,          /**< 已到光幕4, 等光幕5                      */
    BRAKE_ST_SEG5,          /**< 已到光幕5, 等光幕6                      */
    BRAKE_ST_DONE,          /**< 已到光幕6, 时序结束                     */
    BRAKE_ST_FAULT          /**< 过流保护, 全部桥已关                    */
} Svc_Brake_StateTypeDef;

/** 只读快照. */
typedef struct {
    uint8_t  State;             /**< Svc_Brake_StateTypeDef               */
    uint8_t  Enabled;           /**< 1 = 0xA0 使能过                       */
    uint8_t  Level;             /**< 0=低 1=中 2=高                        */
    uint32_t DutyPercent;       /**< 当前档位最大占空比                     */
    uint32_t LsMask;            /**< 最近一次光幕遮挡掩码                   */
    uint32_t OcMask;            /**< 最近一次过流掩码                       */
    uint32_t Reports;           /**< 发出的上报条数                         */
} Svc_Brake_SnapshotTypeDef;

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief  初始化制动模块(PWM + 光幕 + 过流三个 service)。
 * @return SVC_OK / SVC_ERR_HW。
 * @note   必须在网络组件之后调(lwIP 的 GIC 初始化会清处理器表)。
 */
Svc_StatusTypeDef Svc_Brake_Init(void);

/**
 * @brief  建本模块的两条任务: ls(光幕 5 ms 采样) + brake(时序 5 ms)。
 * @return SVC_OK / SVC_ERR_STATE(光幕组件没就绪) / SVC_ERR_HW(建任务失败)。
 * @note   由 App_StartTasks() 调。
 */
Svc_StatusTypeDef Brake_App_Start(void);

/** @brief  读一份快照. */
Svc_StatusTypeDef Svc_Brake_GetSnapshot(Svc_Brake_SnapshotTypeDef *Out);

/** @brief  状态名(日志用). */
const char *Svc_Brake_StateName(Svc_Brake_StateTypeDef State);

/**
 * @brief  处理 0xA0..0xA5(由 inter_app 的命令地图调用)。
 * @return 1 = 报文里有本模块的字节; 0 = 没有(放行)。
 */
int Brake_App_Command(void *Arg, const uint8_t *Data, uint16_t Len, Svc_Net_ReplyTypeDef *R);

#endif /* BRAKE_APP_H */
