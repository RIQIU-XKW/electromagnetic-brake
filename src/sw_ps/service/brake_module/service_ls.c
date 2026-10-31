/**
 *******************************************************************************
 * @file    service_ls.c
 * @brief   ls 业务组件 —— 光幕采样任务 + 上报 + LS 命令表
 *
 * 逻辑照搬重构前的 app/app_ls.c(采样 / 消抖 / 变化才上报 / 上电先报一次),
 * 区别只有两点:
 *   1. 不再往 RS-485 发 APP_CMD_LS_STATUS 帧 —— 帧协议已移除;
 *   2. 状态存在本组件自己的静态实例里, 别人通过 Svc_Ls_GetSnapshot() 读,
 *      不再让 app 持有 App_ContextTypeDef。
 *******************************************************************************
 */

#define LOG_ENABLE 0   /* 关: 亮 = 每次光幕变化打一行 blocked/changed/state */
#define LOG_TAG "ls"
#include "service/monitor_module/log/log.h"

#include "service/brake_module/service_ls.h"
#include "service/inter_module/net_text.h"

#include "bsp/ls/ls.h"          /* 本组件直接驱动光幕输入(service -> bsp) */

#include "osal/osal.h"

/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

static Ls_HandleTypeDef gLs;
static uint8_t          gReady;

static volatile uint8_t  gState;            /**< 消抖后状态(快照)             */
static volatile uint8_t  gRaw;              /**< 最近原始电平(快照)           */
static volatile uint8_t  gChanged;          /**< 最近变化的位(快照)           */
static volatile uint32_t gScans;            /**< 采样次数                     */
static volatile uint32_t gEvents;           /**< 状态变化次数                 */

/* ========================================================================== */
/*  内部函数(static)                                                           */
/* ========================================================================== */

/**
 * @brief  把一次状态变化告诉所有相关方(现在是: 日志 + TCP 广播).
 * @param  State   : 消抖后的状态(1 = 遮挡).
 * @param  Changed : 本次变化的位.
 */
static void Svc_Ls_Publish(uint8_t State, uint8_t Changed)
{
    char     blocked[LS_CHANNELS * 2U + 1U];
    char     delta[LS_CHANNELS * 2U + 1U];

    State   &= LS_MASK;
    Changed &= LS_MASK;

    gState   = State;
    gChanged = Changed;
    gEvents++;

    Ls_FormatMask(State, blocked, (uint16_t)sizeof(blocked));
    Ls_FormatMask(Changed, delta, (uint16_t)sizeof(delta));

    LOG_I("blocked[%s] changed[%s] state 0x%02X events %u\r\n",
          blocked, delta, (unsigned)State, (unsigned)gEvents);

#if (SVC_LS_NOTIFY_TCP != 0U)
    {
        char     text[SVC_NET_MSG_MAX];
        uint16_t pos = 0U;

        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "[zynq] LS 0x");
        pos = Svc_Text_AppendHex2(text, pos, (uint16_t)sizeof(text), State);
        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " blocked[");
        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), blocked);
        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "] changed[");
        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), delta);
        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "]\r\n");

        if (Svc_Net_SendText(text, pos) != SVC_OK) {
            LOG_W("broadcast dropped (net queue full)\r\n");
        }
    }
#else
    (void)delta;
#endif
}

/** @brief  ls 任务: 周期采样 + 消抖, 变化才上报(创建由应用层负责, 见 service_ls.h). */
void Svc_Ls_Task(void *Arg)
{
    uint8_t first = 1U;

    (void)Arg;

    LOG_I("task started: period %u ms, debounce %u samples\r\n",
          (unsigned)SVC_LS_PERIOD_MS, (unsigned)gLs.Debounce);

    for (;;) {
        uint8_t changed = 0U;
        uint8_t state   = Ls_Scan(&gLs, &changed);

        gRaw   = Ls_GetRaw(&gLs);
        gScans = Ls_GetScans(&gLs);

        if (first != 0U) {
            /* 上电先主动报一次, 让 PC 端立刻知道当前光幕状态(和重构前一致). */
            changed = LS_MASK;
            first   = 0U;
        }

        if (changed != 0U) {
            Svc_Ls_Publish(state, changed);
        }

        Osal_DelayMs(SVC_LS_PERIOD_MS);
    }
}

/* ========================================================================== */
/*  对外接口                                                                */
/* ========================================================================== */

Svc_StatusTypeDef Svc_Ls_Init(void)
{
    if (Ls_Init(&gLs, LS_GPIO_BASEADDR, SVC_LS_ACTIVE_LOW) != LS_OK) {
        return SVC_ERR_HW;
    }

    /* 消抖次数由组件决定(驱动只给一个默认值), 这样调采样节奏不用改 bsp */
    gLs.Debounce = (SVC_LS_DEBOUNCE_SCANS != 0U) ? SVC_LS_DEBOUNCE_SCANS : 1U;

    /* 初始状态: 上电读一次, 于是 Changes 只统计真正发生过的变化 */
    gRaw   = Ls_GetRaw(&gLs);
    gState = Ls_GetState(&gLs);

    gReady = 1U;

    LOG_I("ready: %u channels, active-%s, debounce %u\r\n",
          (unsigned)LS_CHANNELS,
          (SVC_LS_ACTIVE_LOW != 0U) ? "low" : "high",
          (unsigned)gLs.Debounce);

    return SVC_OK;
}

uint8_t Svc_Ls_IsReady(void)
{
    return gReady;
}

Svc_StatusTypeDef Svc_Ls_GetSnapshot(Svc_Ls_SnapshotTypeDef *Out)
{
    if (Out == NULL) {
        return SVC_ERR_PARAM;
    }

    Out->Raw     = (uint8_t)gRaw;
    Out->State   = (uint8_t)gState;
    Out->Changed = (uint8_t)gChanged;
    Out->Scans   = gScans;
    Out->Events  = gEvents;

    return SVC_OK;
}
