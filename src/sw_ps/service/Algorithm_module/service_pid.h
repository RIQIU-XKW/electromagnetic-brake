/**
 *******************************************************************************
 * @file    service_pid.h
 * @brief   纯逻辑 PID(浮点, 带抗饱和 + 爬坡限速) —— 不依赖任何硬件, 可 PC 单测
 *
 * 用法(每个控制对象一个状态, 互不共用):
 *     Svc_Pid_StateTypeDef pid;
 *     Svc_Pid_InitSetup(&pid, Kp, Ki, Kd, OutMin, OutMax, SlewStep);
 *     for (;;) {
 *         float out = Svc_Pid_Update(&pid, Target, Actual, Dt);   // Dt 单位: 秒
 *         ...
 *     }
 *
 * 三个要点:
 *   1. 积分项一律用 **float** 累加(不用整数定点), 见 State.Integ;
 *   2. **抗饱和**: 输出已经顶在上/下限、而误差还在往同一方向推的时候, 本拍不累加积分
 *      (条件积分法), 并且把这一拍已经加进去的那部分撤回来;
 *   3. **爬坡限速**: 每拍输出的变化量被限制在 ±SlewStep 之内(单位与输出一致),
 *      所以占空比不会一步拉到顶, 而是随目标电流平滑上升。
 *******************************************************************************
 */

#ifndef SERVICE_PID_H
#define SERVICE_PID_H

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                   */
/* -------------------------------------------------------------------------- */

/** 一个 PID 控制器的状态。每个被控对象各持一个, 不许共用。 */
typedef struct {
    float Kp;           /**< 比例系数(输出单位 / 误差单位)                 */
    float Ki;           /**< 积分系数(输出单位 / (误差单位·秒))            */
    float Kd;           /**< 微分系数(输出单位·秒 / 误差单位)              */

    float Integ;        /**< 积分累加项(浮点)                              */
    float PrevErr;      /**< 上一拍的误差(算微分用)                        */

    float OutMin;       /**< 输出下限(硬钳位, 例如 0 %)                    */
    float OutMax;       /**< 输出上限(硬钳位, 每桥不同, 例如 37 %)         */
    float SlewStep;     /**< 每拍输出最大增量(0 或负数 = 不限速)           */
    float LastOut;      /**< 上一拍的输出(爬坡限速的起点)                  */

    uint8_t Started;    /**< 1 = 已经跑过至少一拍(否则第一拍不做微分/限速)  */
    uint32_t Updates;   /**< 更新次数(诊断用)                              */
} Svc_Pid_StateTypeDef;

/* -------------------------------------------------------------------------- */
/*  接口                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief  配置一个 PID 实例(不改变已有状态; 想清状态用 Svc_Pid_Reset)。
 * @param  OutMin   : 输出下限(硬钳位)。
 * @param  OutMax   : 输出上限(硬钳位)。
 * @param  SlewStep : 每拍最大变化量; <= 0 表示不限速。
 */
void Svc_Pid_InitSetup(Svc_Pid_StateTypeDef *Pid, float Kp, float Ki, float Kd,
                       float OutMin, float OutMax, float SlewStep);

/** @brief  清空积分/历史, 并让输出回到 OutMin(通常 0)。参数保持不变。 */
void Svc_Pid_Reset(Svc_Pid_StateTypeDef *Pid);

/**
 * @brief  跑一拍 PID。
 * @param  Target : 目标值(例如目标电流 A)。
 * @param  Actual : 实际值(例如采样电流 A)。
 * @param  Dt     : 距上一拍的秒数(<= 0 视为不推进, 返回上次输出)。
 * @return 本拍输出(已做硬钳位 + 爬坡限速)。
 */
float Svc_Pid_Update(Svc_Pid_StateTypeDef *Pid, float Target, float Actual, float Dt);

/** @brief  当前输出(最近一次的返回值)。 */
float Svc_Pid_GetOutput(const Svc_Pid_StateTypeDef *Pid);

#endif /* SERVICE_PID_H */
