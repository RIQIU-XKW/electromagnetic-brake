/**
 *******************************************************************************
 * @file    service_pwm.h
 * @brief   PL 功率级(pwm_axi_ip)的初始化、命令表与状态快照
 *
 * 命令表只有一条兜底项(Name = NULL), 内部按 PWMxxx 前缀判断。
 * 不需要单独的任务: 这些命令都是纳秒级寄存器写, 在 net 任务里直接写完即可。
 *******************************************************************************
 */

#ifndef SERVICE_PWM_H
#define SERVICE_PWM_H

#include <stdint.h>

#include "service/service.h"
#include "service/inter_module/service_net.h"

#include "bsp/pwm/pwm.h"        /* Pwm_HandleTypeDef(给 Svc_Pwm_GetHandle 用) */

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                     */
/* -------------------------------------------------------------------------- */

/** 给上层看的只读快照(全部是 bsp 的只读查询, 不影响功率级状态)。 */
typedef struct {
    uint32_t EnableMask;        /**< 半桥使能位掩码(10 位)                    */
    uint32_t FrequencyHz;       /**< 当前开关频率                             */
    uint32_t PeriodCycles;      /**< 当前周期(aclk 周期数)                    */
    uint32_t OcNow;             /**< 过流实时状态位图                         */
    uint32_t OcLatched;         /**< 过流锁存位图(需要 PWMACK 才清)           */
} Svc_Pwm_SnapshotTypeDef;

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                 */
/* -------------------------------------------------------------------------- */

/** @brief  初始化 PWM 驱动(寄存器基址 + aclk)。 */
Svc_StatusTypeDef Svc_Pwm_Init(void);

/** @brief  上电自检: 读一次 ID 寄存器, 值不对说明 AXI 通道/IP 版本有问题。 */
Svc_StatusTypeDef Svc_Pwm_CheckId(void);

/** @brief  取驱动句柄(给同一层的其它组件用, app 层不要用)。 */
Pwm_HandleTypeDef *Svc_Pwm_GetHandle(void);

/** @brief  读一份 PWM 快照。 */
Svc_StatusTypeDef Svc_Pwm_GetSnapshot(Svc_Pwm_SnapshotTypeDef *Out);

/** @brief  过流告警巡检(单次), 由 monitor 组件每秒调一次。AutoAck = 1 时恢复后自动 PWMACK。 */
void Svc_Pwm_AlarmPoll(int AutoAck);

/* -------------------------------------------------------------------------- */
/*  制动控制(给 app/brake_app 用)                                              */
/* -------------------------------------------------------------------------- */

/** 桥个数(= PL PWM IP 的 H 桥数)。 */
#define SVC_PWM_BRIDGE_COUNT    PWM_AXI_BRIDGE_COUNT

/** 默认开关频率(Hz), 编译期常量, 运行期不再改(4 kHz @ 50 MHz aclk = 12500 周期)。 */
#define SVC_PWM_DEFAULT_FREQ_HZ 4000U

/** 默认死区时间(ns), 编译期常量(500 ns @ 50 MHz = 25 周期); 必须有, 否则上下管直通。 */
#define SVC_PWM_DEFAULT_DEADTIME_NS 500U

/** aclk 一个周期 = 20 ns(50 MHz): 把 ns 换成 DEADx 的周期数。 */
#define SVC_PWM_NS_PER_ACLK     20U

/** 制动占空比范围(%): 就是腿 B(BL / Q4)的导通占比。 */
#define SVC_PWM_DUTY_MAX_PERCENT 100U

/** 制动死区可设范围(ns); 10000 ns = 500 个 aclk 周期, 远小于 4 kHz 的一个周期。 */
#define SVC_PWM_DEAD_MAX_NS     10000U

/** @brief  打开/关闭全局调制(CTRL.GLB_EN)。关掉之后所有桥的输出都停。 */
Svc_StatusTypeDef Svc_Pwm_SetGlobalEnable(int Enable);

/**
 * @brief  设某个桥的占空比(0..100 %), 同时把该桥配成对角驱动模式。
 * @note   只"配置"(模式 + 比较值), 不会打开输出 —— 要真的有输出还得 SetBridgeEnable。
 */
Svc_StatusTypeDef Svc_Pwm_SetBridgeDuty(int Bridge, uint32_t Percent);

/** @brief  打开/关闭某个桥的输出(使能它那两路半桥)。 */
Svc_StatusTypeDef Svc_Pwm_SetBridgeEnable(int Bridge, int Enable);

/** @brief  把所有桥的输出关掉(占空比不动), 全局调制保持原样; 过流保护用这个。 */
void Svc_Pwm_DisableAllBridges(void);

/** @brief  读某个桥当前的占空比(0..100)。越界返回 0。 */
uint32_t Svc_Pwm_GetBridgeDuty(int Bridge);

/** @brief  某个桥的输出开着没有(1 = 使能)。越界返回 0。 */
int Svc_Pwm_IsBridgeEnabled(int Bridge);

/** @brief  往串口打一行功率级全状态(IP ID / 调制 / 频率 / 使能 / 过流 / 各桥占空比)。 */
void Svc_Pwm_PrintStatus(void);

/* -------------------------------------------------------------------------- */
/*  制动用法: 腿 A 静态高 + 腿 B 斩波, 五路同步                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief  把一个桥设成"腿 A 静态高(Q1 常开 / Q3 常关) + 腿 B 互补斩波", 占空比 = BL 导通占比。
 * @param  Percent : 0..SVC_PWM_DUTY_MAX_PERCENT(%), 负载平均电压 = Percent/100 * Vbus。
 * @note   只配置, 不打开输出(使能仍由 Svc_Pwm_SetBridgeEnable / SetAllBridgeEnable 管)。
 */
Svc_StatusTypeDef Svc_Pwm_SetBridgeBrakeDuty(int Bridge, uint32_t Percent);

/** @brief  五路桥同步: 同一个占空比一次写到 5 个桥(先定模式再写比较值)。 */
Svc_StatusTypeDef Svc_Pwm_SetAllBrakeDuty(uint32_t Percent);

/** @brief  同 Svc_Pwm_SetBridgeBrakeDuty(), 但占空比用千分比(0..1000) —— 电流闭环要更细的档。 */
Svc_StatusTypeDef Svc_Pwm_SetBridgeBrakePermille(int Bridge, uint32_t Permille);

/** @brief  五路桥同步: 死区(ns)一次写好; 内部按 20 ns/周期换算并夹到 16 位。 */
Svc_StatusTypeDef Svc_Pwm_SetAllDeadtimeNs(uint32_t Ns);

/** @brief  五路桥输出同时开 / 关(gate_en); 全局调制不动。 */
Svc_StatusTypeDef Svc_Pwm_SetAllBridgeEnable(int Enable);

/** @brief  读某个桥某条腿的死区(aclk 周期数); Leg = PWM_AXI_LEG_A / PWM_AXI_LEG_B。 */
uint32_t Svc_Pwm_GetDeadtimeCycles(int Bridge, int Leg);

/** @brief  读某个桥的腿 B 模式(0..3); 越界/没初始化返回 -1。 */
int Svc_Pwm_GetLegBMode(int Bridge);

/* -------------------------------------------------------------------------- */
/*  命令表                                                                     */
/* -------------------------------------------------------------------------- */

/** @brief  PWMxxx 命令的处理函数(兜底命令); 1 = 是本组件处理的。 */
int Svc_Pwm_HandleCommand(void *Arg, const uint8_t *Data, uint16_t Len,
                          Svc_Net_ReplyTypeDef *R);

/** @brief  本组件对外提供的命令表(一条兜底项), 交给 app 注册进 net。 */
extern const Svc_Net_CmdTypeDef Svc_Pwm_Commands[];
extern const uint16_t           Svc_Pwm_CommandCount;

#endif /* SERVICE_PWM_H */
