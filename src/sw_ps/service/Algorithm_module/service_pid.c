/**
 *******************************************************************************
 * @file    service_pid.c
 * @brief   PID 实现: 浮点积分 + 条件积分抗饱和 + 输出爬坡限速
 *******************************************************************************
 */

#include "service/Algorithm_module/service_pid.h"

#include <stddef.h>     /* NULL */

/* ========================================================================== */
/*  内部函数(static)                                                           */
/* ========================================================================== */

/** @brief  把值钳在 [Lo, Hi] 里(上下限写反了就按原值返回, 不做傻事)。 */
static float Svc_Pid_Clamp(float Value, float Lo, float Hi)
{
    if (Hi < Lo) {
        return Value;
    }
    if (Value < Lo) {
        return Lo;
    }
    if (Value > Hi) {
        return Hi;
    }

    return Value;
}

/** @brief  绝对值。 */
static float Svc_Pid_Abs(float V)
{
    return (V < 0.0f) ? -V : V;
}

/* ========================================================================== */
/*  对外接口                                                                   */
/* ========================================================================== */

void Svc_Pid_InitSetup(Svc_Pid_StateTypeDef *Pid, float Kp, float Ki, float Kd,
                       float OutMin, float OutMax, float SlewStep)
{
    if (Pid == NULL) {
        return;
    }

    Pid->Kp       = Kp;
    Pid->Ki       = Ki;
    Pid->Kd       = Kd;
    Pid->OutMin   = OutMin;
    Pid->OutMax   = OutMax;
    Pid->SlewStep = SlewStep;

    Svc_Pid_Reset(Pid);
}

void Svc_Pid_Reset(Svc_Pid_StateTypeDef *Pid)
{
    if (Pid == NULL) {
        return;
    }

    Pid->Integ    = 0.0f;
    Pid->PrevErr  = 0.0f;
    Pid->LastOut  = Pid->OutMin;
    Pid->Started  = 0U;
    Pid->Updates  = 0U;
}

float Svc_Pid_Update(Svc_Pid_StateTypeDef *Pid, float Target, float Actual, float Dt)
{
    float err;
    float integTry;
    float derivative;
    float raw;
    float clamped;
    float out;

    if (Pid == NULL) {
        return 0.0f;
    }

    if (Dt <= 0.0f) {
        return Pid->LastOut;            /* 不推进: 保持上次输出 */
    }

    err = Target - Actual;

    /* ---- 1. 先把这一拍的积分候选值算出来(浮点累加, 不用整数定点) ---- */
    integTry = Pid->Integ + (Pid->Ki * err * Dt);

    /* ---- 2. 微分(第一拍没有历史, 不做微分, 免得开机给一个冲击) ---- */
    if (Pid->Started != 0U) {
        derivative = Pid->Kd * (err - Pid->PrevErr) / Dt;
    } else {
        derivative = 0.0f;
    }

    raw = (Pid->Kp * err) + integTry + derivative;

    /* ---- 3. 硬钳位到该桥的占空比上下限 ---- */
    clamped = Svc_Pid_Clamp(raw, Pid->OutMin, Pid->OutMax);

    /*
     * ---- 4. 抗饱和(条件积分) ----
     * 输出已经顶在上限、而误差还在往正方向推(或者顶在下限、误差还在往负方向推)时,
     * 这一拍的积分**不累加**, 直接撤回到上一拍的值 —— 否则积分项会越积越大,
     * 目标降下来后要很久才退饱和。
     */
    if (((clamped >= Pid->OutMax) && (err > 0.0f)) ||
        ((clamped <= Pid->OutMin) && (err < 0.0f))) {
        integTry = Pid->Integ;          /* 本拍不积分 */
        raw      = (Pid->Kp * err) + integTry + derivative;
        clamped  = Svc_Pid_Clamp(raw, Pid->OutMin, Pid->OutMax);
    }

    /*
     * ---- 5. 爬坡限速 ----
     * 每拍输出最多变化 ±SlewStep(单位与输出一致, 这里是占空比 %), 这样占空比不会
     * 一步拍到上限, 而是随目标电流平滑爬升。SlewStep <= 0 表示不限速。
     */
    out = clamped;

    if (Pid->SlewStep > 0.0f) {
        const float maxUp   = Pid->LastOut + Pid->SlewStep;
        const float maxDown = Pid->LastOut - Pid->SlewStep;

        if (out > maxUp) {
            out = maxUp;
        }
        if (out < maxDown) {
            out = maxDown;
        }

        out = Svc_Pid_Clamp(out, Pid->OutMin, Pid->OutMax);
    }

    /* ---- 6. 收尾: 记状态 ---- */
    if (Pid->Started == 0U) {
        /* 第一拍: 输出从 OutMin 起步, 不要越过爬坡限制 */
        if ((Pid->SlewStep > 0.0f) && (Svc_Pid_Abs(out - Pid->OutMin) > Pid->SlewStep)) {
            out = (out > Pid->OutMin) ? (Pid->OutMin + Pid->SlewStep)
                                      : (Pid->OutMin - Pid->SlewStep);
            out = Svc_Pid_Clamp(out, Pid->OutMin, Pid->OutMax);
        }

        Pid->Started = 1U;
    }

    Pid->Integ   = integTry;
    Pid->PrevErr = err;
    Pid->LastOut = out;
    Pid->Updates++;

    return out;
}

float Svc_Pid_GetOutput(const Svc_Pid_StateTypeDef *Pid)
{
    return (Pid != NULL) ? Pid->LastOut : 0.0f;
}
