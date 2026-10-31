/**
 *******************************************************************************
 * @file    brake_ctl_app.h
 * @brief   制动控制(新方案): 五路 H 桥同步, 占空比/死区由网口帧下发
 *
 * 单路 H 桥的接法(见 bsp/pwm.h 的 PWM_AXI_MODE_LEG_A_STATIC_HIGH):
 *   左桥 Q1(AH/PWM1) 常开、Q3(AL/PWM3) 常关  -> 腿 A 静态高
 *   右桥 Q2(BH/PWM2) 与 Q4(BL/PWM4) 带死区互补斩波 -> 腿 B 按占空比斩波
 * 改 **Q4 的占空比** 就等于改励磁电流: 负载平均电压 = 占空比% x Vbus, 单向电流、慢衰减。
 *
 * 下行命令(沿用单字节段, 语义按新方案):
 *   0xA0 使能(开全局调制, 桥仍关着)      0xA1 启动制动(五桥同时开)
 *   0xA5 停止制动(五桥全关 + 关全局调制)  0xA2/0xA3/0xA4 预设占空比 30/60/90%
 * 下行帧:
 *   [0xDA][占空比高字节][占空比低字节][0xDB]   占空比 0..70  (整数 %, 现场上限 70)
 *   [0xDC][死区高字节][死区低字节][0xDD]       死区 0 或 100..10000 (整数 ns)
 * 上行(2 字节广播):
 *   [0xB0][光幕掩码]  光幕变化(**只上报, 不触发任何制动动作**)
 *   [0xC0][过流掩码]  过流出现 / 恢复
 *
 * 上电默认制动关闭: 五桥输出关、全局调制关、占空比 0、死区 500 ns。
 * 光幕只采样只上报; 制动完全由上位机 → 占空比/启停命令驱动。
 *******************************************************************************
 */

#ifndef BRAKE_CTL_APP_H
#define BRAKE_CTL_APP_H

#include <stdint.h>

#include "service/service.h"
#include "service/inter_module/service_net.h"     /* Svc_Net_ReplyTypeDef */

/* -------------------------------------------------------------------------- */
/*  组件参数                                                                   */
/* -------------------------------------------------------------------------- */

#define BRKCTL_CMD_ENABLE       0xA0U   /**< 使能: 开全局调制(桥不开)          */
#define BRKCTL_CMD_START        0xA1U   /**< 启动制动: 五桥同时打开            */
#define BRKCTL_CMD_PRESET_LOW   0xA2U   /**< 预设占空比 30%                    */
#define BRKCTL_CMD_PRESET_MID   0xA3U   /**< 预设占空比 60%                    */
#define BRKCTL_CMD_PRESET_HIGH  0xA4U   /**< 预设占空比 90%                    */
#define BRKCTL_CMD_STOP         0xA5U   /**< 停止制动: 五桥全关 + 关全局调制    */

#define BRKCTL_FRAME_DUTY_HEAD  0xDAU   /**< 占空比帧头                        */
#define BRKCTL_FRAME_DUTY_TAIL  0xDBU   /**< 占空比帧尾                        */
#define BRKCTL_FRAME_DEAD_HEAD  0xDCU   /**< 死区帧头                          */
#define BRKCTL_FRAME_DEAD_TAIL  0xDDU   /**< 死区帧尾                          */

#define BRKCTL_RPT_LS           0xB0U   /**< [0xB0][光幕遮挡掩码]              */
#define BRKCTL_RPT_OC           0xC0U   /**< [0xC0][过流桥掩码]                */

#define BRKCTL_DUTY_MAX          70U    /**< 占空比上限(%): 现场限制不超过 70      */
#define BRKCTL_DUTY_PRESET_LOW   30U    /**< 预设 低档(0xA2)                       */
#define BRKCTL_DUTY_PRESET_MID   50U    /**< 预设 中档(0xA3)                       */
#define BRKCTL_DUTY_PRESET_HIGH  70U    /**< 预设 高档(0xA4) —— 就是上限           */

#define BRKCTL_DEAD_MAX_NS       10000U /**< 死区上限(ns), 内部再夹到 16 位周期 */
#define BRKCTL_DEAD_MIN_NS       100U   /**< 斩波时死区下限(ns): 1..99 直接拒绝 */
#define BRKCTL_DEAD_DEFAULT_NS   500U   /**< 上电死区(ns)                      */

#define BRKCTL_BRIDGE_COUNT      5U     /**< H 桥数(= 五路同步)                */
#define BRKCTL_LS_COUNT          6U     /**< 光幕路数                          */

#define BRKCTL_TASK_PERIOD_MS    5U     /**< 任务节拍: 光幕/过流巡检 + 应用请求 */
#define BRKCTL_TASK_STACK_WORDS  512U
#define BRKCTL_TASK_PRIO         3U     /**< 和 ads 同级, 保护动作要跟得上      */

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                   */
/* -------------------------------------------------------------------------- */

/** 给 STATUS / 排障看的只读快照。 */
typedef struct {
    uint8_t  Init;              /**< 1 = 组件起来了                        */
    uint8_t  Armed;             /**< 1 = 全局调制开着(0xA0 过)             */
    uint8_t  On;                /**< 1 = 五桥输出开着(0xA1 后, 0xA5 前)     */
    uint8_t  Fault;             /**< 1 = 过流保护动作过(要 0xA0+0xA1 复位) */
    uint32_t DutyPercent;       /**< 当前占空比(%)                         */
    uint32_t DeadNs;            /**< 当前死区(ns)                          */
    uint32_t LsMask;            /**< 最近一次光幕遮挡掩码                  */
    uint32_t Reports;           /**< 已发出的上报条数                      */
    uint32_t DutyFrames;        /**< 收到的占空比帧数                      */
    uint32_t DeadFrames;        /**< 收到的死区帧数                        */
} BrkCtl_SnapshotTypeDef;

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief  初始化: 光幕/过流组件 + 五路桥配成"腿 A 静态高 + 腿 B 斩波" + 制动关闭。
 * @note   必须在 Svc_Pwm_Init() 之后调用(上游 App_Init 已经保证)。
 */
Svc_StatusTypeDef BrakeCtl_App_Init(void);

/** @brief  建 ls 采样任务 + 本模块的控制任务。 */
Svc_StatusTypeDef BrakeCtl_App_Start(void);

/**
 * @brief  强制回到"制动关闭"态: 五桥全关 + 关全局调制(占空比/死区设置保留)。
 * @note   给"上位机刚连上网口"用: 默认态就该是不制动, 上一次留下的输出不许跟着新连接继续跑。
 *         在调用者上下文里直接写寄存器(纳秒级), 可以从中断以外的任何任务里调。
 */
void BrakeCtl_App_ForceOff(void);

/** @brief  读一份快照。 */
Svc_StatusTypeDef BrakeCtl_App_GetSnapshot(BrkCtl_SnapshotTypeDef *Out);

/**
 * @brief  命令处理(由 inter_app 的命令地图调用)。
 * @return 1 = 是本模块处理的, 0 = 不是(交给下一个兜底项)。
 */
int BrakeCtl_App_Command(void *Arg, const uint8_t *Data, uint16_t Len,
                         Svc_Net_ReplyTypeDef *R);

#endif /* BRAKE_CTL_APP_H */
