/**
 *******************************************************************************
 * @file    brake_app.c
 * @brief   制动业务: 命令(0xA0..0xA5) + 非阻塞制动时序 + 上报(0xB0/0xC0)
 *
 *   app/brake_app.c       命令 / 状态机 / 上报 / 过流保护决策(本文件)
 *   service/brake_module  service_pwm(桥+占空比) / service_ls(光幕) / service_oc(过流)
 *   bsp/pwm, bsp/ls, bsp/oc
 *
 * 本文件不 include bsp 头, 也不用 Osal_DelayMs: 时序全靠"周期任务 + 状态机"。
 *******************************************************************************
 */

#define LOG_ENABLE 1   /* 0xA0/0xA1/每过一道光幕/过流出现与恢复各打一行 */
#define LOG_TAG "brake"
#include "service/monitor_module/log/log.h"

#include "app/brake_app.h"

#include "service/inter_module/service_net.h"
#include "service/brake_module/service_pwm.h"
#include "service/brake_module/service_ls.h"
#include "service/brake_module/service_oc.h"

#include "osal/osal.h"

/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

static volatile Svc_Brake_StateTypeDef gState;
static uint8_t  gEnabled;               /**< 0xA0 使能过没有                     */
static uint8_t  gLevel;                 /**< 档位: 0 低 / 1 中 / 2 高            */
static uint32_t gDutyPercent;           /**< 当前档位的最大占空比(%)             */

static uint32_t gLsMask;                /**< 此刻的光幕遮挡掩码(bit0 = 光幕1)   */
static uint32_t gLsReported;            /**< 最近一次上报出去的光幕掩码          */
static uint32_t gLsHeldWarned;          /**< 已提醒过"光幕低着但时序没跑"的掩码  */
static uint32_t gOcMask;                /**< 最近一次过流掩码                    */
static uint8_t  gOcEventReported;       /**< 已补报过的累计位(短脉冲补报用)      */
static uint16_t gOcHoldTicks;           /**< 过流"保持显示"剩余拍数               */
static uint8_t  gOcLastSrc = 0xFFU;     /**< 上次上报出去的过流源(0xFF = 还没报) */
static uint32_t gReports;               /**< 上报条数(排障)                      */

/** 客户端数轮询间隔: 20 拍 × 5 ms = 100 ms。 */
#define BRAKE_CLI_POLL_TICKS   20U
static uint8_t  gClientsSeen = 0xFFU;   /**< 上次看到的客户端数(0xFF = 还没查过) */
static uint8_t  gCliPoll;

static Osal_TaskHandleTypeDef gTask;
static uint8_t  gTaskReady;

/* ========================================================================== */
/*  上报(2 字节定长: [码][掩码])                                                */
/* ========================================================================== */

/** @brief  此刻连着几个 TCP 客户端(给上报日志用)。 */
static uint32_t Brake_NetClients(void)
{
    Svc_Net_SnapshotTypeDef net;

    if (Svc_Net_GetSnapshot(&net) != SVC_OK) {
        return 0U;
    }

    return (uint32_t)net.Clients;
}

/**
 * @brief  客户端数变化时, 把当前状态(过流 + 光幕)补报一遍。
 *
 * 上报是"值变了才发", 而新连上来的上位机从"正常"开始 —— 不补报就会一直显示旧状态。
 * 做法: 把上次上报值置成不可能的值, 下一拍判定必然认为"变了", 于是重发一次。
 */
static void Svc_Brake_PushStateOnNewClient(void)
{
    uint32_t clients = Brake_NetClients();

    if (clients == gClientsSeen) {
        return;
    }

    gClientsSeen = clients;

    if (clients != 0U) {
        gOcLastSrc  = 0xFFU;
        gLsReported = 0xFFFFFFFFU;

        LOG_I("client connected (cli=%u) - current state will be pushed\r\n",
              (unsigned)clients);
    } else {
        LOG_I("all clients gone (cli=0)\r\n");
    }
}

/**
 * @brief  发一条 2 字节上报(0xB0 光幕 / 0xC0 过流)。
 * @return SVC_OK = 已进网络队列(不代表客户端收到), 其它 = 没排上队。
 */
static Svc_StatusTypeDef Svc_Brake_Report(uint8_t Code, uint8_t Mask)
{
    uint8_t              frame[2];
    Svc_StatusTypeDef    st;

    frame[0] = Code;
    frame[1] = Mask;

    st = Svc_Net_SendBytes(frame, 2U);
    if (st == SVC_OK) {
        gReports++;
    }

    return st;
}

/** @brief  光幕变化上报(0xB0)。 */
static Svc_StatusTypeDef Svc_Brake_ReportLs(uint32_t Mask)
{
    return Svc_Brake_Report(BRAKE_RPT_LS, (uint8_t)(Mask & 0xFFU));
}

/** @brief  过流告警上报(0xC0)。 */
static Svc_StatusTypeDef Svc_Brake_ReportOc(uint32_t Mask)
{
    return Svc_Brake_Report(BRAKE_RPT_OC, (uint8_t)(Mask & 0xFFU));
}

/* ========================================================================== */
/*  桥的操作(经 service_pwm)                                                   */
/* ========================================================================== */

/** @brief  开一个桥: 先设占空比, 再打开输出(两步都要, 只设占空比不会输出)。 */
static void Svc_Brake_BridgeOn(int Bridge, uint32_t Percent)
{
    (void)Svc_Pwm_SetBridgeDuty(Bridge, Percent);
    (void)Svc_Pwm_SetBridgeEnable(Bridge, 1);
}

/** @brief  关一个桥(只关输出, 占空比留着)。 */
static void Svc_Brake_BridgeOff(int Bridge)
{
    (void)Svc_Pwm_SetBridgeEnable(Bridge, 0);
}

/** @brief  关全部桥 + 关全局调制。 */
static void Svc_Brake_AllOff(void)
{
    int b;

    for (b = 0; b < (int)BRAKE_BRIDGE_COUNT; b++) {
        Svc_Brake_BridgeOff(b);
    }

    (void)Svc_Pwm_SetGlobalEnable(0);
}

/* ========================================================================== */
/*  档位                                                                       */
/* ========================================================================== */

/** @brief  档位 -> 最大占空比(%)。 */
static uint32_t Svc_Brake_DutyOfLevel(uint8_t Level)
{
    switch (Level) {
    case 0U:  return BRAKE_DUTY_LOW_PERCENT;
    case 1U:  return BRAKE_DUTY_MID_PERCENT;
    case 2U:  return BRAKE_DUTY_HIGH_PERCENT;
    default:  return BRAKE_DUTY_LOW_PERCENT;
    }
}

/** @brief  桥刚打开、还没等到自己那一路光幕时的预置占空比(%)。 */
static uint32_t Svc_Brake_PrestartDuty(void)
{
    return BRAKE_DUTY_PRESTART_PERCENT;
}

/** @brief  状态名。 */
const char *Svc_Brake_StateName(Svc_Brake_StateTypeDef State)
{
    switch (State) {
    case BRAKE_ST_IDLE:      return "idle";
    case BRAKE_ST_ENABLED:   return "enabled";
    case BRAKE_ST_WAIT_LS1:  return "wait-ls1";
    case BRAKE_ST_SEG1:      return "seg1";
    case BRAKE_ST_SEG2:      return "seg2";
    case BRAKE_ST_SEG3:      return "seg3";
    case BRAKE_ST_SEG4:      return "seg4";
    case BRAKE_ST_SEG5:      return "seg5";
    case BRAKE_ST_DONE:      return "done";
    case BRAKE_ST_FAULT:     return "fault";
    default:                 return "?";
    }
}

/* ========================================================================== */
/*  状态机(非阻塞: 每拍只做"当前该做的一件事")                                  */
/* ========================================================================== */

/**
 * @brief  按光幕电平推进一步: 该升满的桥升满、该关的桥关掉。
 *
 * @param  BlockedMask : 此刻被遮挡的路(电平掩码, bit0 = 光幕1)。
 *
 * 时序(光幕 n 拉低 = 车到第 n 路): 桥 n 升到档位最大值, 桥 n+1 按预置打开,
 * 桥 n-1 关闭; 光幕 6 到达时关桥 5, 时序结束。
 *
 * ⚠ 传电平而不是"本拍新出现的边沿": 光幕在启动之前就已经低着(车停在光幕上/上电
 *   就被挡住)时, 边沿会被吃掉, 之后再也看不到 → 桥不升档、不接力。
 * ⚠ 状态只许前进: 遮挡到的最高一路没有超过"已经走到的那一路"时直接返回,
 *   否则车抖动/信号闪断会把状态拽回去, 已经升满的桥会被写回预置值。
 */
static void Svc_Brake_StepOnLs(uint32_t BlockedMask)
{
    uint32_t mask = BlockedMask & ((1UL << (uint32_t)BRAKE_LS_COUNT) - 1UL);
    int      passed;            /* 已经走到第几路(0 = 还没过任何一路) */
    int      hi = 0;            /* 此刻遮挡到的最高那一路 */
    int      n;

    if (mask == 0UL) {
        return;                 /* 一路都没遮挡 */
    }

    /* IDLE/ENABLED/FAULT/DONE 都不推进(没起时序 / 保护已关桥 / 这一轮已结束) */
    if (gState == BRAKE_ST_WAIT_LS1) {
        passed = 0;
    } else if ((gState >= BRAKE_ST_SEG1) && (gState <= BRAKE_ST_SEG5)) {
        passed = ((int)gState - (int)BRAKE_ST_SEG1) + 1;
    } else {
        return;
    }

    for (n = (int)BRAKE_LS_COUNT; n >= 1; n--) {
        if ((mask & (1UL << (uint32_t)(n - 1))) != 0UL) {
            hi = n;
            break;
        }
    }

    if (hi <= passed) {
        return;                 /* 只许前进 */
    }

    /* 从"上一路 + 1"逐路补到 hi: 中间有光幕被跳过时也能补齐(每步都幂等) */
    for (n = passed + 1; n <= hi; n++) {
        /* ① 这一路自己的桥: 升到档位最大值 */
        if ((n - 1) < (int)BRAKE_BRIDGE_COUNT) {
            (void)Svc_Pwm_SetBridgeDuty(n - 1, gDutyPercent);
            LOG_I("ls%d low -> bridge%d = %u%%\r\n", n, n, (unsigned)gDutyPercent);
        }

        /* ② 下一个桥: 按预置占空比打开 */
        if (n < (int)BRAKE_BRIDGE_COUNT) {
            Svc_Brake_BridgeOn(n, Svc_Brake_PrestartDuty());
            LOG_I("ls%d low -> bridge%d open at %u%% (prestart)\r\n",
                  n, n + 1, (unsigned)Svc_Brake_PrestartDuty());
        }

        /* ③ 上一个桥: 关掉 */
        if ((n - 2) >= 0) {
            Svc_Brake_BridgeOff(n - 2);
            LOG_I("ls%d low -> bridge%d closed\r\n", n, n - 1);
        }
    }

    gState = (hi >= (int)BRAKE_LS_COUNT)
             ? BRAKE_ST_DONE
             : (Svc_Brake_StateTypeDef)((int)BRAKE_ST_SEG1 + (hi - 1));

    Svc_Pwm_PrintStatus();
}

/**
 * @brief  过流保护: 关全部桥 + 进 FAULT。
 *
 * 最高优先级, 任何时刻过流都立即动作, 不自动恢复(重新制动要 0xA0 + 0xA1)。
 * OcMask 只用来打印哪几路; 上报由周期任务统一发(见 Svc_Brake_Task)。
 */
static void Svc_Brake_Protect(uint32_t OcMask)
{
    Svc_Brake_AllOff();
    gState = BRAKE_ST_FAULT;

    (void)OcMask;               /* LOG 关掉时参数就没人用了, 显式吃掉免警告 */
    LOG_E("overcurrent src 0x%02X - all bridges off, state FAULT\r\n", (unsigned)OcMask);
}

/* ========================================================================== */
/*  周期任务(唯一的时间推进者, 5 ms 一拍)                                       */
/* ========================================================================== */

/**
 * @brief  制动周期任务: 采过流 -> 上报 + 保护 -> 采光幕 -> 上报 + 推状态机。
 *
 * 一个 Osal_DelayMs 都没有: 段与段之间的"等待"就是"状态没变, 下一拍再来"。
 */
static void Svc_Brake_Task(void *Arg)
{
    (void)Arg;

    LOG_I("task started (period %u ms)\r\n", (unsigned)BRAKE_TASK_PERIOD_MS);

    for (;;) {
        Svc_Ls_SnapshotTypeDef ls;
        uint8_t                newEvent   = 0U;   /* 这一拍发现新的一次过流 */
        uint8_t                live       = 0U;
        uint8_t                evMask     = 0U;

        /* ---- 1. 采样过流(每拍都采) -------------------------------------- */
        Svc_Oc_Poll();

        /* ---- 1b. 客户端数变了就补报当前状态 ------------------------------ */
        if (++gCliPoll >= BRAKE_CLI_POLL_TICKS) {
            gCliPoll = 0U;
            Svc_Brake_PushStateOnNewClient();
        }

        /* ---- 2. 过流: 算上报值 + 保护 ------------------------------------
         * 显示用"保持窗口": 真实过流的引脚只低几微秒, 严格跟电平报会让红点只亮
         * 一拍。窗口内每次过流都把窗口顶满, 故障没排除就不会消失; 窗口过了且引脚干净
         * 才报 src=0 变绿, 并顺手清掉过流残留(硬件锁存 + 软件累计位)。
         */
        live    = (Svc_Oc_IsAlarm() != 0);
        evMask  = (uint8_t)(Svc_Oc_GetEventMask() & 0xFFU);

        if (live != 0U) {
            uint32_t srcNow = Svc_Oc_GetNowMask() & 0xFFU;

            gOcMask      = (srcNow != 0U) ? srcNow : (uint32_t)evMask;
            gOcHoldTicks = BRAKE_OC_HOLD_TICKS;
        } else if ((evMask != 0U) && (evMask != gOcEventReported)) {
            newEvent         = 1U;
            gOcEventReported = evMask;
            gOcMask          = (uint32_t)evMask;
            gOcHoldTicks     = BRAKE_OC_HOLD_TICKS;
        } else if (gOcHoldTicks > 0U) {
            gOcHoldTicks--;
        } else {
            if (gOcMask != 0U) {
                (void)Svc_Oc_Acknowledge();
                gOcEventReported = 0U;
                gOcMask          = 0U;
            }
        }

        if (gOcMask != gOcLastSrc) {
            Svc_StatusTypeDef sent = Svc_Brake_ReportOc(gOcMask);

            gOcLastSrc = gOcMask;

            /* st = 进没进队列, rpt = 已发条数, cli = 此刻几个客户端(0 = 没人在听) */
            if (gOcMask != 0U) {
                LOG_E("0xC0 reported src=0x%02X st=%d rpt=%u cli=%u\r\n",
                      (unsigned)gOcMask, (int)sent, (unsigned)gReports,
                      (unsigned)Brake_NetClients());
            } else {
                LOG_I("0xC0 cleared src=0 st=%d rpt=%u cli=%u\r\n",
                      (int)sent, (unsigned)gReports, (unsigned)Brake_NetClients());
            }
        }

        if (((live != 0U) || (newEvent != 0U)) && (gState != BRAKE_ST_FAULT)) {
            /* 引脚还低, 或刚发现短脉冲 -> 关桥保护(已 FAULT 不重复做, 免得刷屏) */
            Svc_Brake_Protect(gOcMask);
        }

        if (live != 0) {
            /* 还在过流: 这一拍不推光幕状态机, 免得一边保护一边往下一段开桥 */
            (void)Osal_TaskNotifyTake(BRAKE_TASK_PERIOD_MS);
            continue;
        }

        /* ---- 3. 光幕: 采样 + 变化就上报 0xB ---------------------------- */
        (void)Svc_Ls_GetSnapshot(&ls);
        gLsMask = (uint32_t)ls.State;

        if (gLsMask != gLsReported) {
            const Svc_StatusTypeDef sent = Svc_Brake_ReportLs(gLsMask);

            gLsReported = gLsMask;

            LOG_I("ls mask 0x%02X reported (st=%d rpt=%u cli=%u)\r\n",
                  (unsigned)(gLsMask & 0xFFU), (int)sent,
                  (unsigned)gReports, (unsigned)Brake_NetClients());
        }

        /* ---- 4. 推状态机(传当前电平) ------------------------------------ */
        if ((gState == BRAKE_ST_IDLE) || (gState == BRAKE_ST_ENABLED) ||
            (gState == BRAKE_ST_FAULT) || (gState == BRAKE_ST_DONE)) {
            /* 有遮挡但时序没在跑: 提醒一次(同一掩码只提醒一次), 否则现场无从判断 */
            if ((gLsMask != 0U) && (gLsMask != gLsHeldWarned)) {
                gLsHeldWarned = gLsMask;

                LOG_W("ls mask 0x%02X is low but the sequence is '%s' - the relay "
                      "does not move until 0xA0 + 0xA1 (state fault/done needs a "
                      "restart)\r\n",
                      (unsigned)(gLsMask & 0xFFU), Svc_Brake_StateName(gState));
            }
        } else {
            gLsHeldWarned = 0U;

            Svc_Brake_StepOnLs(gLsMask);
        }

        (void)Osal_TaskNotifyTake(BRAKE_TASK_PERIOD_MS);
    }
}

/* ========================================================================== */
/*  命令(单字节 0xA0..0xA5)                                                     */
/* ========================================================================== */

/** @brief  0xA0: 使能(开全局调制, 等 0xA1)。 */
static void Svc_Brake_CmdEnable(void)
{
    (void)Svc_Pwm_SetGlobalEnable(1);

    gEnabled = 1U;
    if (gState == BRAKE_ST_IDLE) {
        gState = BRAKE_ST_ENABLED;
    }

    LOG_I("0xA0 enable: global modulation ON, state %s\r\n",
          Svc_Brake_StateName(gState));

    Svc_Pwm_PrintStatus();
}

/** @brief  0xA1: 启动时序(清过流残留 -> 关全部桥 -> 开桥1 预置 -> 等光幕1)。 */
static void Svc_Brake_CmdStart(void)
{
    int b;

    if (gEnabled == 0U) {
        LOG_W("0xA1 start ignored: send 0xA0 (enable) first\r\n");
        return;
    }

    /* 只在引脚此刻还拉着过流时拒绝: 引脚干净了就当"明确的重启意图" */
    if (Svc_Oc_IsAlarm() != 0) {
        LOG_W("0xA1 start ignored: oc_n is STILL low (src=0x%02X) - "
              "clear the overcurrent first\r\n",
              (unsigned)Svc_Oc_GetNowMask());
        Svc_Pwm_PrintStatus();
        return;
    }

    (void)Svc_Oc_Acknowledge();
    gOcLastSrc       = 0xFFU;
    gOcEventReported = 0U;
    gOcMask          = 0U;
    gOcHoldTicks     = 0U;      /* 不清的话, 上一轮的保持窗口会把这一轮顶成"过流中" */

    gDutyPercent = Svc_Brake_DutyOfLevel(gLevel);

    for (b = 0; b < (int)BRAKE_BRIDGE_COUNT; b++) {
        Svc_Brake_BridgeOff(b);
    }

    (void)Svc_Pwm_SetGlobalEnable(1);

    Svc_Brake_BridgeOn(0, Svc_Brake_PrestartDuty());

    gState = BRAKE_ST_WAIT_LS1;

    LOG_I("0xA1 start: bridge1 open at %u%% (prestart), level %u -> max %u%%, waiting ls1\r\n",
          (unsigned)Svc_Brake_PrestartDuty(), (unsigned)gLevel, (unsigned)gDutyPercent);

    Svc_Pwm_PrintStatus();
}

/** @brief  0xA5: 关闭制动(关全部桥 + 清过流状态 + 回 idle)。 */
static void Svc_Brake_CmdStop(void)
{
    Svc_Brake_AllOff();

    (void)Svc_Oc_Acknowledge();
    gOcLastSrc       = 0xFFU;
    gOcEventReported = 0U;
    gOcMask          = 0U;
    gOcHoldTicks     = 0U;

    gEnabled = 0U;
    gState   = BRAKE_ST_IDLE;

    LOG_I("0xA5 stop: all bridges off, overcurrent cleared, state idle\r\n");
}

/**
 * @brief  处理 0xA0..0xA5(由 inter_app 的命令地图调用)。
 *
 * 按字节流处理: 上位机可能把几个字节粘在一个报文里发过来, 逐个字节判断;
 * 完全不认识的字节返回 0, 让别的模块的兜底项继续认领。
 * 回话长度一律传 0(用 strlen), 不要手写字数。
 */
int Brake_App_Command(void *Arg, const uint8_t *Data, uint16_t Len,
                                   Svc_Net_ReplyTypeDef *R)
{
    uint16_t i;
    uint8_t  handled = 0U;

    (void)Arg;

    if ((Data == NULL) || (Len == 0U)) {
        return 0;
    }

    for (i = 0U; i < Len; i++) {
        const uint8_t cmd = Data[i];

        if ((cmd < BRAKE_CMD_ENABLE) || (cmd > BRAKE_CMD_STOP)) {
            /* 一个都没处理过 -> 整段不是制动的; 处理过 -> 后面的字节不归我管 */
            if (handled == 0U) {
                return 0;
            }
            break;
        }

        handled = 1U;

        switch (cmd) {
        case BRAKE_CMD_ENABLE:
            Svc_Brake_CmdEnable();
            (void)Svc_Net_Reply(R, "0xA0 brake enable\r\n", 0U);
            break;

        case BRAKE_CMD_START:
            Svc_Brake_CmdStart();
            (void)Svc_Net_Reply(R, "0xA1 brake start\r\n", 0U);
            break;

        case BRAKE_CMD_LEVEL_LOW:
            gLevel       = 0U;
            gDutyPercent = Svc_Brake_DutyOfLevel(gLevel);
            (void)Svc_Net_Reply(R, "0xA2 brake level LOW (max 30%)\r\n", 0U);
            break;

        case BRAKE_CMD_LEVEL_MID:
            gLevel       = 1U;
            gDutyPercent = Svc_Brake_DutyOfLevel(gLevel);
            (void)Svc_Net_Reply(R, "0xA3 brake level MID (max 60%)\r\n", 0U);
            break;

        case BRAKE_CMD_LEVEL_HIGH:
            gLevel       = 2U;
            gDutyPercent = Svc_Brake_DutyOfLevel(gLevel);
            (void)Svc_Net_Reply(R, "0xA4 brake level HIGH (max 90%)\r\n", 0U);
            break;

        case BRAKE_CMD_STOP:
            Svc_Brake_CmdStop();
            (void)Svc_Net_Reply(R, "0xA5 brake stop\r\n", 0U);
            break;

        default:
            break;
        }
    }

    return (handled != 0U) ? 1 : 0;
}

/* ========================================================================== */
/*  对外接口                                                                   */
/* ========================================================================== */

Svc_StatusTypeDef Svc_Brake_Init(void)
{
    int b;

    /* OC 必须排在 PWM 之后: 它要借 PWM 的句柄读过流状态位 */
    if (Svc_Pwm_Init() != SVC_OK) {
        LOG_E("pwm init failed\r\n");
        return SVC_ERR_HW;
    }

    if (Svc_Pwm_CheckId() != SVC_OK) {
        LOG_E("pwm ID check failed - AXI/IP version problem\r\n");
        return SVC_ERR_HW;
    }

    if (Svc_Ls_Init() != SVC_OK) {
        LOG_E("ls (light curtain) init failed\r\n");
        return SVC_ERR_HW;
    }

    if (Svc_Oc_Init() != SVC_OK) {
        LOG_E("oc (overcurrent) init failed\r\n");
        return SVC_ERR_HW;
    }

    /* 上电态: 5 个桥配成对角驱动、占空比 0、全部不使能(线圈两端无电压) */
    for (b = 0; b < (int)BRAKE_BRIDGE_COUNT; b++) {
        (void)Svc_Pwm_SetBridgeDuty(b, 0U);
        (void)Svc_Pwm_SetBridgeEnable(b, 0);
    }

    (void)Svc_Pwm_SetGlobalEnable(0);

    gState       = BRAKE_ST_IDLE;
    gEnabled     = 0U;
    gLevel       = 0U;
    gDutyPercent = Svc_Brake_DutyOfLevel(gLevel);
    gLsMask      = 0U;
    gLsReported  = 0U;
    gOcMask      = 0U;
    gOcLastSrc       = 0xFFU;
    gOcEventReported = 0U;
    gOcHoldTicks     = 0U;
    gReports     = 0U;

    LOG_I("ready: %u bridges (all diagonal, outputs off), %u light curtains, "
          "level %u (max %u%%)\r\n",
          (unsigned)BRAKE_BRIDGE_COUNT, (unsigned)BRAKE_LS_COUNT,
          (unsigned)gLevel, (unsigned)gDutyPercent);

    /* 上电态是 OFF / en 0x000 / duty 全 0, 这是正常的: 要输出得先 0xA0 再 0xA1 */
    LOG_I("boot state (before any 0xA0/0xA1 - this is normal, not a fault):\r\n");
    Svc_Pwm_PrintStatus();

    return SVC_OK;
}

/**
 * @brief  建本模块的两条任务: ls(光幕 5 ms 采样) + brake(时序 5 ms)。
 *
 * 光幕没就绪就直接失败: 制动时序完全靠光幕推进, 没有采样就没有任何进展。
 * (别在这里加 `#if (APP_ENABLE_LS != 0U)`: 本文件没有包含 app_config.h, 那个宏在
 *  这里未定义, `#if` 会按 0 处理把整段静默编译掉 —— 构建开了 -Wundef, 但更别写。)
 */
Svc_StatusTypeDef Brake_App_Start(void)
{
    if (Svc_Ls_IsReady() == 0U) {
        LOG_E("ls component not ready - no ls task, no brake sequence "
              "(see the ls/pwm init error above)\r\n");
        return SVC_ERR_STATE;
    }

    if (Osal_TaskCreate("ls", Svc_Ls_Task, NULL,
                        SVC_LS_STACK_WORDS, SVC_LS_PRIO, NULL) != OSAL_OK) {
        LOG_E("ls task creation failed (no light curtain sampling)\r\n");
        return SVC_ERR_HW;
    }

    if (gTaskReady != 0U) {
        return SVC_OK;                  /* 起过了, 幂等 */
    }

    if (Osal_TaskCreate("brake", Svc_Brake_Task, NULL,
                        BRAKE_TASK_STACK_WORDS, BRAKE_TASK_PRIO, &gTask) != OSAL_OK) {
        LOG_E("brake task creation failed\r\n");
        return SVC_ERR_HW;
    }

    gTaskReady = 1U;

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Brake_GetSnapshot(Svc_Brake_SnapshotTypeDef *Out)
{
    if (Out == NULL) {
        return SVC_ERR_PARAM;
    }

    Out->State       = (uint8_t)gState;
    Out->Enabled     = gEnabled;
    Out->Level       = gLevel;
    Out->DutyPercent = gDutyPercent;
    Out->LsMask      = gLsMask;
    Out->OcMask      = gOcMask;
    Out->Reports     = gReports;

    return SVC_OK;
}
