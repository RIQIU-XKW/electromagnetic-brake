/**
 *******************************************************************************
 * @file    service_current_loop.h
 * @brief   电流负反馈闭环(5 路独立 PID): 目标电流 -> 每桥占空比
 *
 * 一个控制周期(10 ms)做这些事:
 *     读 5 路采样电流(A) -> 每桥自己的 PID(目标相同, 上限各不相同) -> 写各桥占空比
 *
 * 每桥一套独立状态(不许共用), 占空比硬上限各有各的(%):
 *     桥1 0..37   桥2 0..40   桥3 0..28   桥4 0..45   桥5 0..75
 *
 * 目标电流由上位机帧 {0xBA, mA_lo, mA_hi, 0xAB} 下发, **5 个桥共用同一个目标**;
 * 目标为 0(或未下发过)时闭环自动把占空比爬到 0 并关掉桥输出。
 *
 * ⚠ 和"占空比模式"(app/brake_ctl_app.c: 0xA1 / 占空比帧)抢同一个功率级, 规矩是
 *   **最后一条命令说了算**:
 *     · 收到电流指令 > 0  -> 闭环接管(自己开调制 + 开五桥);
 *     · 0xA5 关制动 / 过流保护 / 收到占空比帧 -> 闭环立刻放手(Svc_CurLoop_Stop)。
 *
 * 积分用 float; 输出被钳到上下限时停止积分(抗饱和); 每拍占空比增量受
 * SVC_CURLOOP_SLEW_STEP_PER_CYCLE(%) 限制, 所以占空比是平滑爬上去的。
 *******************************************************************************
 */

#ifndef SERVICE_CURRENT_LOOP_H
#define SERVICE_CURRENT_LOOP_H

#include <stdint.h>

#include "service/service.h"
#include "service/Algorithm_module/service_pid.h"

/* -------------------------------------------------------------------------- */
/*  组件参数                                                                   */
/* -------------------------------------------------------------------------- */

/** 桥数(= 5 路独立闭环)。 */
#define SVC_CURLOOP_BRIDGES         5U

/** 电流采样/指令的满量程(A)。 */
#define SVC_CURLOOP_CURRENT_MAX_A   15.0f

/** 控制周期(ms): 和 ADS 的采样周期一致, 一拍一个新反馈。 */
#define SVC_CURLOOP_PERIOD_MS       10U

/** 任务栈/优先级。 */
#define SVC_CURLOOP_STACK_WORDS     512U
#define SVC_CURLOOP_PRIO            3U

/** 每桥占空比硬上限(%): 显式常量, 不要写成 100。 */
#define SVC_CURLOOP_DUTY_MAX_P1     37.0f
#define SVC_CURLOOP_DUTY_MAX_P2     40.0f
#define SVC_CURLOOP_DUTY_MAX_P3     28.0f
#define SVC_CURLOOP_DUTY_MAX_P4     45.0f
#define SVC_CURLOOP_DUTY_MAX_P5     75.0f

/** 占空比下限(%)。 */
#define SVC_CURLOOP_DUTY_MIN        0.0f

/** 每控制周期占空比最大增量(%/拍): 10 ms 一拍, 2% 就是 200%/s 的爬升速度。 */
#define SVC_CURLOOP_SLEW_STEP_PER_CYCLE  2.0f

/** PID 默认参数(输出单位 = 占空比 %, 误差单位 = A)。现场可调。 */
#define SVC_CURLOOP_KP              1.50f
#define SVC_CURLOOP_KI              3.00f
#define SVC_CURLOOP_KD              0.00f

/** 目标电流低于这个值(A)就认为"不要电流": 闭环退到 0 并关桥输出。 */
#define SVC_CURLOOP_IDLE_A          0.02f

/**
 * 闭环投入时使用的死区时间(ns)。
 *
 * 为什么要有这个默认值: 电流闭环是"小占空比也要出电流"的场合, 死区越长, 低占空比
 * 段的失真越大; 而占空比模式那边默认 500 ns。闭环接管时把死区按 100 ns(硬件允许的
 * 最小值)设一遍, 免得上一轮留下的长死区影响调节品质。
 *
 * ⚠ 只在**投入的那一刻**写一次, 不是每拍都写 —— 上位机在 PWM 栏发的死区帧(0xDC)
 *   随时能改, 改完不会被闭环又刷回 100 ns。
 */
#define SVC_CURLOOP_DEAD_NS_DEFAULT 100U

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                   */
/* -------------------------------------------------------------------------- */

/** 给上层/排障看的只读快照。 */
typedef struct {
    uint8_t  Ready;                                 /**< 1 = 组件初始化完成          */
    uint8_t  Active;                                /**< 1 = 正在闭环输出            */
    float    TargetA;                               /**< 当前目标电流(A)             */
    float    ActualA[SVC_CURLOOP_BRIDGES];           /**< 各桥实际电流(A)             */
    float    DutyPercent[SVC_CURLOOP_BRIDGES];       /**< 各桥当前占空比(%)           */
    float    LimitPercent[SVC_CURLOOP_BRIDGES];      /**< 各桥占空比上限(%)           */
    uint32_t Cycles;                                /**< 跑过多少个控制周期          */
    uint32_t Cmds;                                  /**< 收到过多少条电流指令        */
} Svc_CurLoop_SnapshotTypeDef;

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                   */
/* -------------------------------------------------------------------------- */

/** @brief  初始化 5 路 PID(每桥一套状态, 上限各不相同)。 */
Svc_StatusTypeDef Svc_CurLoop_Init(void);

/** @brief  控制任务入口(10 ms 一拍); 任务由应用层创建。 */
void Svc_CurLoop_Task(void *Arg);

/**
 * @brief  设目标电流(A), 5 个桥共用。
 * @param  Amps : 0..SVC_CURLOOP_CURRENT_MAX_A; 越界会被夹紧。
 */
Svc_StatusTypeDef Svc_CurLoop_SetTargetA(float Amps);

/** @brief  当前目标电流(A)。 */
float Svc_CurLoop_GetTargetA(void);

/**
 * @brief  "外部接管式"停止: 目标清零 + 5 路 PID 复位, 立刻放手(不爬坡、不碰使能)。
 *
 * 用在 0xA5 停制动、过流保护、以及切回占空比模式这三种场合 —— 调用方负责功率级
 * 的开关, 本函数只把自己的状态清干净。
 * 滑块拖到 0 那种"自己退出"走 Svc_CurLoop_SetTargetA(0), 它会按爬坡速率退到 0。
 */
Svc_StatusTypeDef Svc_CurLoop_Stop(void);

/** @brief  读一份快照。 */
Svc_StatusTypeDef Svc_CurLoop_GetSnapshot(Svc_CurLoop_SnapshotTypeDef *Out);

#endif /* SERVICE_CURRENT_LOOP_H */
