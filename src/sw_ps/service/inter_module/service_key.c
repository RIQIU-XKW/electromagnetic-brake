/**
 *******************************************************************************
 * @file    service_key.c
 * @brief   key 业务组件实现 —— 消抖状态机 + 动作派发 + 状态变化上报
 *
 * 状态机(每 SVC_KEY_POLL_MS 走一次, 纯整数, 见 tools/key_test 的宿主机测试):
 *
 *     raw = 读引脚电平
 *     if (raw == Last) Count++  else { Last = raw; Count = 1; }
 *     if (Count >= DEBOUNCE && Stable != raw) {
 *         Stable = raw;                     <- 稳定电平变了
 *         if (raw == ACTIVE) -> 触发一次"按下"
 *     }
 *
 * 要点:
 *   - **只在"稳定电平变成按下"的那一次动作** -> 长按不重复(Count 够了之后 Stable 已经
 *     等于 raw 了, 条件不再成立);
 *   - Count 是"连续相同"的计数, 抖动时 Last 一变就把它归 1, 所以抖不过 DEBOUNCE 次
 *     就凑不出一次稳定变化;
 *   - 没有用"时间戳"而是用"采样次数": 采样周期是固定的, 次数就是时间, 而且整数计数
 *     比时间戳好测(不用假装时间流逝)。
 *
 * 上报(2026-10-16 起): 每次消抖后的**电平变化**都调一次 sink(Svc_Key_SetEventSink),
 *   应用层(app/inter_app.c)把它变成 2 字节广播 [0xAA][掩码] 给上位机。**本文件不认识
 *   网口协议** —— 只报"哪个键现在按着"(SVC_KEY_EVENT_*)。
 *******************************************************************************
 */

#define LOG_ENABLE 1
#define LOG_TAG "key"
#include "service/monitor_module/log/log.h"

#include "service/inter_module/service_key.h"


#include "bsp/ps_key/ps_key.h"
#include "bsp/ps_rst/ps_rst.h"

#include "osal/osal.h"


/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

static Svc_KeyStateTypeDef gKey1;       /**< KEY1 = MIO10 = 球 E9 = 软复位      */
static Svc_KeyStateTypeDef gKey2;       /**< KEY2 = MIO11 = 球 C6 = 重连网络    */

static volatile uint8_t  gReady;
static volatile uint8_t  gArmed;        /**< 过了上电保护期没有                  */
static volatile uint32_t gKey1Presses;
static volatile uint32_t gKey2Presses;
static volatile uint32_t gResets;
static volatile uint32_t gNetRestarts;

/** 状态变化的上报口(应用层挂的, 见 Svc_Key_SetEventSink)。 */
static Svc_Key_EventSinkTypeDef gSink;

/** 上一次报出去的掩码 + "报过没有"(第一次不管变没变都要报一次基线)。 */
static uint8_t gReportedMask;
static uint8_t gReportDone;

/** 运行参数(由 Svc_Key_Init 从 app 传进来)。 */
static uint32_t gPollMs      = 20U;
static uint32_t gArmDelayMs  = 3000U;
static uint8_t  gDebounce    = 3U;
static uint8_t  gActiveLevel;           /**< 按下时的电平(0 = 按下为低)         */

/**
 * KEY2 的动作开关(0 = 保留不使用: 引脚照读、照计数, 但不重启网络)。
 *
 * 默认给 0: 这个组件刚加进来时是先"只接线不动作"的 —— 按键卡住/按错也不会有任何
 * 副作用, 但 KEYSTAT 里能看出接线对不对。app 那边由 APP_KEY2_RESTART_ENABLE 决定。
 */
static uint8_t  gKey2Enable;

/* ========================================================================== */
/*  消抖状态机                                                                 */
/* ========================================================================== */

/** @brief  把状态机推进一步。 @return 1 = 这一次"稳定地按下"了(边沿)。 */
static uint8_t Svc_Key_Step(Svc_KeyStateTypeDef *st, uint8_t Raw)
{
    if (Raw == st->Last) {
        if (st->Count < 0xFFU) {
            st->Count++;
        }
    } else {
        st->Last  = Raw;
        st->Count = 1U;
    }

    if ((st->Count >= gDebounce) && (st->Stable != Raw)) {
        st->Stable = Raw;

        return (Raw == gActiveLevel) ? 1U : 0U;
    }

    return 0U;
}

/* ========================================================================== */
/*  上报(状态变化 -> 应用层的 sink)                                             */
/* ========================================================================== */

/** @brief  当前**按着**的键(按 SVC_KEY_EVENT_* 取或)。 */
static uint8_t Svc_Key_PressedMask(void)
{
    uint8_t mask = 0U;

    if (gKey1.Stable == gActiveLevel) {
        mask = (uint8_t)(mask | SVC_KEY_EVENT_KEY1);
    }

    if (gKey2.Stable == gActiveLevel) {
        mask = (uint8_t)(mask | SVC_KEY_EVENT_KEY2);
    }

    return mask;
}

/**
 * @brief  电平有变化(或者还没报过基线)就回调一次 sink。
 *
 * @note   "变化才报"和光幕那条 0xB0 一个道理: 按键是慢变化的物理量, 20 ms 轮询下
 *         每拍都报会把网口和上位机的日志刷爆。
 */
static void Svc_Key_Report(void)
{
    const uint8_t mask = Svc_Key_PressedMask();

    if ((gReportDone != 0U) && (mask == gReportedMask)) {
        return;
    }

    gReportedMask = mask;
    gReportDone   = 1U;

    if (gSink != NULL) {
        gSink(mask);
    }
}

/* ========================================================================== */
/*  动作                                                                       */
/* ========================================================================== */

/** @brief  KEY1 的动作: 复位 PS(重启软件 + 重新跑硬件初始化)。 */
static void Svc_Key_DoReset(void)
{
    gResets++;

    /*
     * 先停掉采集/录制再复位: 复位会把 DDR 控制器一起重启, 正录着的数据如果还在
     * 内存缓冲里就丢了。落盘走的是 eMMC 组件, 这里给它留一点时间。
     *
     * 用 Osal_DelayMs 在按键任务里睡 —— 这个任务优先级最低, 睡一下不影响别人;
     * 而且复位之后本来什么都不会再发了, 不差这几百毫秒。
     */
    Osal_DelayMs(SVC_KEY_RESET_SETTLE_MS);

    PsRst_SoftReset();      /* 正常不返回 */

    /* 走到这里说明复位没生效。别把按键任务卡死在这儿, 记一笔继续跑。 */
    LOG_E("reset did not take effect - still running\r\n");
}

/** @brief  KEY2 的动作: 重启 TCP 服务端(上位机重连即可)。 */
static void Svc_Key_DoNetRestart(void)
{
    gNetRestarts++;

    /*
     * 先等 SVC_KEY_NET_NOTICE_MS 再请求重启, 让"按键按下了"那条上报有机会上网。
     *
     * 为什么要等: 上报是走 Svc_Net_SendBytes **排队**给 net 任务广播的(2 ms 一拍),
     * 而 net 任务每拍是"先处理重启请求(踢掉客户端)、再排空广播队列" —— 不等的话
     * 客户端会在收到上报之前就被断开, 上位机看到的就是"莫名其妙掉线"。
     * 50 ms 对"人按下按键 -> 网络重连"这件事完全感觉不到。
     */
    Osal_DelayMs(SVC_KEY_NET_NOTICE_MS);

    if (Svc_Net_RequestRestart() != SVC_OK) {
        LOG_W("could not request the tcp server restart\r\n");
        gNetRestarts--;
        return;
    }

    LOG_W("KEY2: requested the tcp server restart (clients will be dropped)\r\n");
}

/**
 * @brief  KEY2 被按下时走这里: 开关没开就只记一条日志。
 *
 * 计数(gKey2Presses)在调用方加, 所以"保留不使用"期间依然能从 KEYSTAT 看出
 * 按键有没有被按到、接线对不对 —— 只是不会动网络。
 */
static void Svc_Key_DoKey2(void)
{
    if (gKey2Enable == 0U) {
        LOG_I("KEY2 pressed (action disabled - kept for later use)\r\n");
        return;
    }

    Svc_Key_DoNetRestart();
}

/* ========================================================================== */
/*  任务函数                                                                     */
/* ========================================================================== */

void Svc_Key_Task(void *Arg)             /* 创建由应用层负责, 见 service_key.h */
{
    uint32_t elapsed = 0U;

    (void)Arg;

    /* 上电保护期: 这段时间里按键只读不响应(见 service_key.h 里的第 3 条防护) */
    while (elapsed < gArmDelayMs) {
        Osal_DelayMs(gPollMs);
        elapsed += gPollMs;

        /* 就算在保护期里, 也把状态机推着走 —— 这样"按键现在是按下的"能从 KEYSTAT
         * 里看出来, 便于判断"板子起来就复位"是不是按键卡住了。 */
        (void)Svc_Key_Step(&gKey1, PsKey_ReadLevel(PS_KEY_MIO_KEY1));
        (void)Svc_Key_Step(&gKey2, PsKey_ReadLevel(PS_KEY_MIO_KEY2));
    }

    gArmed = 1U;
    LOG_I("armed after %u ms; keys are live now\r\n", (unsigned)elapsed);

    /* 基线: 不管变没变先报一次, 上位机一挂上就知道两个键现在是什么状态 */
    Svc_Key_Report();

    for (;;) {
        const uint8_t k1 = Svc_Key_Step(&gKey1, PsKey_ReadLevel(PS_KEY_MIO_KEY1));
        const uint8_t k2 = Svc_Key_Step(&gKey2, PsKey_ReadLevel(PS_KEY_MIO_KEY2));

        /*
         * ⚠ 顺序: **先上报, 再做动作**。动作会把网络踢掉(KEY1 复位、KEY2 重启 TCP
         *   服务端), 反过来的话上位机只会看到"掉线", 不知道是按键干的。
         */
        Svc_Key_Report();

        if (k1 != 0U) {
            gKey1Presses++;
            LOG_W("KEY1 pressed -> soft reset\r\n");
            Svc_Key_DoReset();
        }

        if (k2 != 0U) {
            gKey2Presses++;
            Svc_Key_DoKey2();
        }

        Osal_DelayMs(gPollMs);
    }
}

/* ========================================================================== */
/*  动作接口(给应用层用: 命令 KEYRESET / KEYNET 搬到了 app/inter_app.c)          */
/* ========================================================================== */

/*
 * ⚠ 2026-10-16: 命令处理从这里搬走了 —— 服务层只提供"能力", 不认命令
 *   (和 brake_module / Mover_module / Samp_module 一个路子)。
 *   app/inter_app.c 那边的 KEYRESET / KEYNET 最终落到下面这两个函数。
 */

/** @brief 软复位(和按 KEY1 同一个动作: 计数 + 给落盘留时间 + 真的复位 PS). 正常不返回. */
void Svc_Key_RequestReset(void)
{
    Svc_Key_DoReset();
}

/**
 * @brief 重启 TCP 服务(和按 KEY2 同一个动作).
 * @return 1 = 已经请求重启; 0 = **这个功能没开**(Cfg.Key2RestartEnable = 0),
 *         调用方要如实告诉敲命令的人, 不能默默什么都不做。
 */
int Svc_Key_RequestNetRestart(void)
{
    if (gKey2Enable == 0U) {
        return 0;
    }

    Svc_Key_DoNetRestart();

    return 1;
}

Svc_StatusTypeDef Svc_Key_SetEventSink(Svc_Key_EventSinkTypeDef Sink)
{
    gSink = Sink;       /* 单次写指针, 运行中换也可以(见 service_key.h) */

    return SVC_OK;
}

/* ========================================================================== */
/*  对外接口                                                                */
/* ========================================================================== */

Svc_StatusTypeDef Svc_Key_Init(const Svc_Key_ConfigTypeDef *Cfg)
{
    if (Cfg == NULL) {
        return SVC_ERR_PARAM;
    }

    /* 参数: 0 / 越界的都回落到默认值, 免得一个笔误把消抖变成 0 次(等于没消抖) */
    gPollMs      = (Cfg->PollMs != 0U)     ? Cfg->PollMs     : 20U;
    gArmDelayMs  = Cfg->ArmDelayMs;
    gDebounce    = (Cfg->Debounce != 0U)   ? Cfg->Debounce   : 3U;
    gActiveLevel = (Cfg->ActiveLow != 0U)  ? 0U              : 1U;
    gKey2Enable  = (Cfg->Key2RestartEnable != 0U) ? 1U       : 0U;

    if (PsKey_Init() != PS_KEY_OK) {
        LOG_E("PS key driver init failed - the key component is skipped\r\n");
        return SVC_ERR_HW;
    }

    /*
     * 状态机初值 = 当前电平。这样上电时如果按键**已经是按下的**, 不会被当成一次
     * "按下" —— 否则上电瞬间就先来一发(复位键的话就不是"重启"而是"起不来"了)。
     * Count 直接给满, 表示"这个电平已经稳定了"。
     */
    gKey1.Last = gKey1.Stable = PsKey_ReadLevel(PS_KEY_MIO_KEY1);
    gKey2.Last = gKey2.Stable = PsKey_ReadLevel(PS_KEY_MIO_KEY2);
    gKey1.Count = gDebounce;
    gKey2.Count = gDebounce;

    gReady = 1U;

    /*
     * 保护期**从 0 重新开始**。
     *
     * 别省这一句: gArmed 是模块级静态量, 不在这里清零的话, "重新初始化"之后会沿用
     * 上一次的"已过保护期"状态 —— 那保护期就形同虚设了(比如先起来过一次、后来因为
     * 别的原因又调了一次 Init, 第二次就没有保护期了)。
     * 这个坑是 tools/key_test 第 [9] 节抓出来的。
     */
    gArmed = 0U;

    LOG_I("ready: poll=%u ms, debounce=%u, active=%s, arm delay=%u ms, "
          "KEY2 action=%s\r\n",
          (unsigned)gPollMs, (unsigned)gDebounce,
          (gActiveLevel == 0U) ? "low" : "high", (unsigned)gArmDelayMs,
          (gKey2Enable != 0U) ? "enabled (tcp restart)" : "disabled (kept for later use)");

    return SVC_OK;
}

uint8_t Svc_Key_IsReady(void)
{
    return gReady;
}

Svc_StatusTypeDef Svc_Key_GetSnapshot(Svc_Key_SnapshotTypeDef *Out)
{
    if (Out == NULL) {
        return SVC_ERR_PARAM;
    }

    Out->Ready       = gReady;
    Out->Armed       = gArmed;
    Out->Key1Level   = gKey1.Stable;
    Out->Key2Level   = gKey2.Stable;
    Out->Key1Presses = gKey1Presses;
    Out->Key2Presses = gKey2Presses;
    Out->Resets      = gResets;
    Out->NetRestarts = gNetRestarts;
    Out->Key2Enabled = gKey2Enable;

    return SVC_OK;
}
