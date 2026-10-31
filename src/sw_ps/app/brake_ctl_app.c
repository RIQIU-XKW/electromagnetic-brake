/**
 *******************************************************************************
 * @file    brake_ctl_app.c
 * @brief   制动控制(新方案): 五路 H 桥同步 + 占空比/死区帧下发 + 光幕只上报
 *
 * 数据流:
 *   net 任务(命令) -> gPendDuty / gPendDead  -> 本模块 5 ms 任务 -> service_pwm -> PL
 *   ls 组件(5 ms 采样) -> 快照 -> 本模块任务发现变化 -> 广播 [0xB0][掩码]
 *   service_oc(每拍巡检) -> 过流 -> 立刻全关 + 广播 [0xC0][掩码]
 *
 * 旧的光幕驱动时序状态机(app/brake_app.c)保留在仓库里但**不再参与编译/创建任务**,
 * 见文件末尾的说明。
 *******************************************************************************
 */

#define LOG_ENABLE 1   /* 启停/占空比/死区/过流各打一行, 光幕只在上报时不刷串口 */
#define LOG_TAG "brkctl"
#include "service/monitor_module/log/log.h"

#include "app/brake_ctl_app.h"

#include "service/brake_module/service_pwm.h"
#include "service/brake_module/service_oc.h"
#include "service/brake_module/service_ls.h"
#include "service/inter_module/service_net.h"
#include "service/inter_module/net_text.h"         /* Svc_Text_Append* */
#include "service/monitor_module/led/service_led.h"
#include "service/Algorithm_module/service_current_loop.h"   /* 互锁: 电流闭环让位 */

#include "osal/osal.h"

/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

static uint8_t  gInit;                      /**< 1 = 组件建起来了               */
static uint8_t  gArmed;                     /**< 1 = 全局调制开着               */
static uint8_t  gOn;                        /**< 1 = 五桥输出开着               */
static uint8_t  gFault;                     /**< 1 = 过流保护动作过             */

static uint32_t gDutyPercent;               /**< 当前占空比(%)                  */
static uint32_t gDeadNs = BRKCTL_DEAD_DEFAULT_NS;

static uint32_t gPendDutyPercent;           /**< 待应用的占空比(命令刚写的)     */
static uint32_t gPendDeadNs;                /**< 待应用的死区                   */
static volatile uint8_t gPendDuty;          /**< 1 = 有新的占空比要应用         */
static volatile uint8_t gPendDead;          /**< 1 = 有新的死区要应用           */

static uint32_t gLsMask;                    /**< 最近一次读到的光幕掩码         */
static uint8_t  gLsValid;                   /**< 0 = 还没读过, 第一次直接上报   */
static uint32_t gOcLastMask = 0xFFU;        /**< 上次上报出去的过流掩码(0xFF=没报) */
static uint32_t gReports;                   /**< 上报条数                       */
static uint32_t gDutyFrames;                /**< 收到的占空比帧数               */
static uint32_t gDeadFrames;                /**< 收到的死区帧数                 */

/* ========================================================================== */
/*  内部函数(static)                                                           */
/* ========================================================================== */

/** @brief  发一条 2 字节上报(0xB0 / 0xC0)。 */
static Svc_StatusTypeDef BrkCtl_Report(uint8_t Code, uint8_t Mask)
{
    uint8_t           frame[2];
    Svc_StatusTypeDef st;

    frame[0] = Code;
    frame[1] = Mask;

    st = Svc_Net_SendBytes(frame, 2U);
    if (st == SVC_OK) {
        gReports++;
    }

    return st;
}

/** @brief  五桥全关 + 关全局调制(制动的"关闭"态, 也是过流保护动作)。 */
static void BrkCtl_AllOff(void)
{
    (void)Svc_Pwm_SetAllBridgeEnable(0);
    (void)Svc_Pwm_SetGlobalEnable(0);

    gOn    = 0U;
    gArmed = 0U;

    /*
     * 顺手把电流闭环也停掉。
     *
     * 为什么必须有这一句: 电流闭环的任务每 10 ms 一拍, 只要目标 > 0 就会自己
     * "开调制 + 开五桥"。要是 0xA5 只关自己的功率级不动它的目标, 下一拍桥又被
     * 闭环打开 —— 点了"关闭制动"却关不掉, 这是安全问题。
     */
    (void)Svc_CurLoop_Stop();
}

/** @brief  把当前占空比应用到五路(五路同一个值)。 */
static void BrkCtl_ApplyDuty(uint32_t Percent)
{
    if (Svc_Pwm_SetAllBrakeDuty(Percent) != SVC_OK) {
        LOG_E("apply duty %u%% failed\r\n", (unsigned)Percent);
        return;
    }

    gDutyPercent = Percent;
}

/** @brief  把死区应用到五路。 */
static void BrkCtl_ApplyDead(uint32_t Ns)
{
    if (Svc_Pwm_SetAllDeadtimeNs(Ns) != SVC_OK) {
        LOG_E("apply dead time %u ns failed\r\n", (unsigned)Ns);
        return;
    }

    gDeadNs = Ns;
}

/** @brief  读光幕快照, 变化就广播 [0xB0](只上报, 不触发任何制动动作)。 */
static void BrkCtl_PollLs(void)
{
    Svc_Ls_SnapshotTypeDef ls;

    if (Svc_Ls_GetSnapshot(&ls) != SVC_OK) {
        return;
    }

    if ((gLsValid != 0U) && (ls.State == (uint8_t)gLsMask)) {
        return;
    }

    gLsValid = 1U;
    gLsMask  = (uint32_t)ls.State;

    (void)BrkCtl_Report(BRKCTL_RPT_LS, (uint8_t)(gLsMask & 0xFFU));

    LOG_I("ls 0x%02X -> reported (no brake action)\r\n", (unsigned)(gLsMask & 0xFFU));
}

/** @brief  过流巡检: 一出现(实时或锁存)就全关并上报, 恢复也上报一条。 */
static void BrkCtl_PollOc(void)
{
    uint32_t now = Svc_Oc_GetNowMask() & 0xFFU;
    uint32_t lat = Svc_Oc_GetLatchedMask() & 0xFFU;
    uint32_t src = (now | lat) & 0xFFU;

    if (src == gOcLastMask) {
        return;                     /* 状态没变, 不重复上报 */
    }

    if (src != 0U) {
        /* 保护动作: 五桥立刻关掉(硬件其实在 ~2 ns 内已经关了, 这里把软件状态对齐) */
        (void)Svc_Pwm_DisableAllBridges();
        (void)Svc_CurLoop_Stop();       /* 闭环也停: 否则它下一拍就把桥重新打开 */
        gOn    = 0U;
        gArmed = 0U;
        gFault = 1U;

        LOG_E("overcurrent src=0x%02X -> all bridges off, need 0xA0 + 0xA1\r\n",
              (unsigned)src);
    } else {
        LOG_I("overcurrent cleared\r\n");
    }

    gOcLastMask = src;
    (void)BrkCtl_Report(BRKCTL_RPT_OC, (uint8_t)src);
}

/** @brief  把命令线程写下的"待应用"值落到硬件上。 */
static void BrkCtl_ApplyPending(void)
{
    if (gPendDead != 0U) {
        gPendDead = 0U;
        BrkCtl_ApplyDead(gPendDeadNs);
        LOG_I("dead time -> %u ns\r\n", (unsigned)gDeadNs);
    }

    if (gPendDuty != 0U) {
        gPendDuty = 0U;
        BrkCtl_ApplyDuty(gPendDutyPercent);
        LOG_I("duty -> %u%% (five bridges)\r\n", (unsigned)gDutyPercent);
    }
}

/* ========================================================================== */
/*  任务函数                                                                   */
/* ========================================================================== */

static void BrkCtl_Task(void *Arg)
{
    (void)Arg;

    for (;;) {
        Svc_Oc_Poll();              /* 采一拍过流(置 NowMask/EventMask) */
        BrkCtl_ApplyPending();      /* 先应用新占空比/死区, 再巡检 */
        BrkCtl_PollOc();
        BrkCtl_PollLs();

        Osal_DelayMs((uint32_t)BRKCTL_TASK_PERIOD_MS);
    }
}

/* ========================================================================== */
/*  命令处理                                                                   */
/* ========================================================================== */

/** @brief  回一行文本(只回给发命令的那个客户端)。 */
static void BrkCtl_Reply(Svc_Net_ReplyTypeDef *R, const char *Text)
{
    if (R != NULL) {
        (void)Svc_Net_Reply(R, Text, 0U);
    }
}

/** @brief  设占空比(0..BRKCTL_DUTY_MAX %)。 */
static void BrkCtl_SetDuty(uint16_t Value, Svc_Net_ReplyTypeDef *R)
{
    char text[96];
    uint16_t pos = 0U;

    gDutyFrames++;

    if (Value > (uint16_t)BRKCTL_DUTY_MAX) {
        BrkCtl_Reply(R, "brake ERR: duty must be 0..70 (%)\r\n");
        return;
    }

    gPendDutyPercent = (uint32_t)Value;
    gPendDuty        = 1U;

    /*
     * 上位机来改占空比 = 要回占空比模式, 电流闭环让位(否则闭环下一拍又把这个值改掉)。
     * 就算当前闭环没在跑, 这句也是空操作。
     */
    (void)Svc_CurLoop_Stop();

    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "brake duty ");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), (uint32_t)Value);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                            (gOn != 0U) ? " % applied to 5 bridges\r\n"
                                        : " % armed (send 0xA0 + 0xA1 to output)\r\n");
    (void)Svc_Net_Reply(R, text, pos);
}

/** @brief  设死区(ns)。 */
static void BrkCtl_SetDead(uint16_t Value, Svc_Net_ReplyTypeDef *R)
{
    char     text[96];
    uint16_t pos = 0U;

    gDeadFrames++;

    if (Value > (uint16_t)BRKCTL_DEAD_MAX_NS) {
        BrkCtl_Reply(R, "brake ERR: dead time must be 0..10000 (ns)\r\n");
        return;
    }

    /*
     * 0 只允许在"占空比 0"时出现(那时右桥不斩波); 真正斩波时死区不许小于 100 ns ——
     * 1..99 ns 在这套硬件上等于没插死区, 互补驱动下就是上下管直通, 直接拒绝。
     */
    if ((Value != 0U) && (Value < (uint16_t)BRKCTL_DEAD_MIN_NS)) {
        BrkCtl_Reply(R, "brake ERR: dead time must be 0 (duty 0) or >= 100 ns\r\n");
        return;
    }

    gPendDeadNs = (uint32_t)Value;
    gPendDead   = 1U;

    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "brake dead time ");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), (uint32_t)Value);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " ns (5 bridges)\r\n");
    (void)Svc_Net_Reply(R, text, pos);
}

int BrakeCtl_App_Command(void *Arg, const uint8_t *Data, uint16_t Len,
                         Svc_Net_ReplyTypeDef *R)
{
    uint16_t i       = 0U;
    uint16_t value   = 0U;
    int      handled = 0;

    (void)Arg;

    if ((Data == NULL) || (Len == 0U)) {
        return 0;
    }

    /*
     * **整条报文按顺序扫完** —— 不能"处理完一条就 return":
     * 上位机一口气发 [0xDA 帧][0xDC 帧][0xA0][0xA1] 时, TCP 常常把它们合并成一段,
     * 早先的实现在第一条帧(或第一条命令)处理完就返回, 后面的 0xA0/0xA1 被直接丢掉,
     * 现象就是"点了启动制动、占空比和死区都变了, 但桥一直不开"(2026-10-27 踩过)。
     */
    while (i < Len) {
        const uint8_t b = Data[i];

        /* ---- 帧: [head][hi][lo][tail] ---- */
        if ((b == BRKCTL_FRAME_DUTY_HEAD) || (b == BRKCTL_FRAME_DEAD_HEAD)) {
            const uint8_t tail = (b == BRKCTL_FRAME_DUTY_HEAD) ? BRKCTL_FRAME_DUTY_TAIL
                                                               : BRKCTL_FRAME_DEAD_TAIL;

            if ((uint16_t)(i + 4U) > Len) {
                BrkCtl_Reply(R, "brake ERR: frame truncated (need head + 16bit + tail)\r\n");
                return 1;
            }

            if (Data[i + 3U] != tail) {
                BrkCtl_Reply(R, "brake ERR: bad frame tail (0xDA needs 0xDB, 0xDC needs 0xDD)\r\n");
                handled = 1;
                i++;                        /* 只跳过帧头, 后面的字节继续按命令扫 */
                continue;
            }

            value = (uint16_t)(((uint16_t)Data[i + 1U] << 8) | (uint16_t)Data[i + 2U]);
            if (b == BRKCTL_FRAME_DUTY_HEAD) {
                BrkCtl_SetDuty(value, R);
            } else {
                BrkCtl_SetDead(value, R);
            }
            handled = 1;
            i = (uint16_t)(i + 4U);
            continue;
        }

        /* ---- 单字节命令 0xA0..0xA5 ---- */
        switch (b) {
        case BRKCTL_CMD_ENABLE:
            (void)Svc_Pwm_SetGlobalEnable(1);
            gArmed = 1U;
            gFault = 0U;
            LOG_I("0xA0 enable: global modulation ON (bridges still off)\r\n");
            BrkCtl_Reply(R, "brake 0xA0 enabled (modulation ON); send 0xA1 to output\r\n");
            handled = 1;
            i++;
            continue;

        case BRKCTL_CMD_START:
            /*
             * 占空比模式接管: 先把电流闭环按住(否则两条链路每 10 ms 抢一次占空比)。
             * 之后要回到电流模式, 上位机再发一条电流指令就行。
             */
            (void)Svc_CurLoop_Stop();
            (void)Svc_Pwm_SetGlobalEnable(1);
            (void)Svc_Pwm_SetAllBrakeDuty(gDutyPercent);       /* 五路同步 */
            if (Svc_Pwm_SetAllBridgeEnable(1) != SVC_OK) {
                BrkCtl_Reply(R, "brake ERR: bridge enable failed\r\n");
                return 1;
            }
            gArmed = 1U;
            gOn    = 1U;
            {
                Svc_Pwm_SnapshotTypeDef pwm;

                (void)Svc_Pwm_GetSnapshot(&pwm);
                LOG_I("0xA1 start: 5 bridges ON at %u%%, en 0x%03X, period %u\r\n",
                      (unsigned)gDutyPercent, (unsigned)(pwm.EnableMask & 0x3FFU),
                      (unsigned)pwm.PeriodCycles);
            }
            Svc_Pwm_PrintStatus();   /* 一行全状态: 里面有每桥的 mode(应为 3,3,3,3,3) */
            BrkCtl_Reply(R, "brake 0xA1 started: 5 bridges ON\r\n");
            handled = 1;
            i++;
            continue;

        case BRKCTL_CMD_PRESET_LOW:
        case BRKCTL_CMD_PRESET_MID:
        case BRKCTL_CMD_PRESET_HIGH:
            gPendDutyPercent = (b == BRKCTL_CMD_PRESET_LOW)  ? BRKCTL_DUTY_PRESET_LOW
                             : (b == BRKCTL_CMD_PRESET_MID)  ? BRKCTL_DUTY_PRESET_MID
                                                             : BRKCTL_DUTY_PRESET_HIGH;
            gPendDuty = 1U;
            BrkCtl_Reply(R, "brake preset duty queued (5 bridges)\r\n");
            handled = 1;
            i++;
            continue;

        case BRKCTL_CMD_STOP:
            /* 先清过流锁存(它会给 GLB_EN 一个 0->1 边沿), 再把调制和桥全关掉 */
            (void)Svc_Oc_Acknowledge();
            BrkCtl_AllOff();
            LOG_I("0xA5 stop: 5 bridges off, modulation off\r\n");
            BrkCtl_Reply(R, "brake 0xA5 stopped (all bridges off)\r\n");
            handled = 1;
            i++;
            continue;

        default:
            i++;                            /* 别人的命令, 跳过继续扫 */
            continue;
        }
    }

    return handled;
}

/* ========================================================================== */
/*  对外接口                                                                   */
/* ========================================================================== */

Svc_StatusTypeDef BrakeCtl_App_Init(void)
{
    int b;

    /*
     * PWM 必须最先起来(整个应用里只有这一处调 Svc_Pwm_Init):
     *   - 下面要用 Svc_Pwm_SetBridgeBrakeDuty / SetAllDeadtimeNs;
     *   - 过流组件(Svc_Oc_Init)要借 PWM 的句柄读 STATUS 的过流位。
     * ⚠ 漏了这两行 -> App_Init 返回 APP_ERR_HW -> main 停在死循环里, 任务一个都不建,
     *   表现就是"JTAG 下载完连不上网口"(2026-10-30 踩过)。
     */
    if (Svc_Pwm_Init() != SVC_OK) {
        LOG_E("pwm init failed\r\n");
        return SVC_ERR_HW;
    }

    if (Svc_Pwm_CheckId() != SVC_OK) {
        LOG_E("pwm ID check failed - AXI/IP version problem\r\n");
        return SVC_ERR_HW;
    }

    /* 1. 五路桥: 模式 = 腿 A 静态高 + 腿 B 斩波, 占空比 0, 死区默认值 */
    for (b = 0; b < (int)BRKCTL_BRIDGE_COUNT; b++) {
        if (Svc_Pwm_SetBridgeBrakeDuty(b, 0U) != SVC_OK) {
            LOG_E("bridge %d mode setup failed\r\n", b);
            return SVC_ERR_HW;
        }
    }
    (void)Svc_Pwm_SetAllDeadtimeNs(BRKCTL_DEAD_DEFAULT_NS);

    /* 2. 上电默认制动关闭: 桥全关 + 全局调制关 */
    BrkCtl_AllOff();

    gDutyPercent     = 0U;
    gDeadNs          = BRKCTL_DEAD_DEFAULT_NS;
    gPendDutyPercent = 0U;
    gPendDeadNs      = BRKCTL_DEAD_DEFAULT_NS;
    gFault           = 0U;
    gLsValid         = 0U;
    gOcLastMask      = 0xFFU;

    /* 3. 光幕与过流组件 */
    if (Svc_Ls_Init() != SVC_OK) {
        LOG_E("ls (light curtain) init failed\r\n");
        return SVC_ERR_HW;
    }

    if (Svc_Oc_Init() != SVC_OK) {
        LOG_E("oc (overcurrent) init failed\r\n");
        return SVC_ERR_HW;
    }

    gInit = 1U;
    LOG_I("ready: 5 bridges leg-A-static-high / leg-B chop, duty 0%%, dead %u ns, brake OFF\r\n",
          (unsigned)gDeadNs);

    return SVC_OK;
}

Svc_StatusTypeDef BrakeCtl_App_Start(void)
{
    if (gInit == 0U) {
        LOG_E("not initialised - no tasks created\r\n");
        return SVC_ERR_STATE;
    }

    if (Svc_Ls_IsReady() == 0U) {
        LOG_E("ls component not ready\r\n");
        return SVC_ERR_HW;
    }

    /* 光幕采样任务(5 ms, 只采样 + 组件自己的文本上报) */
    if (Osal_TaskCreate("ls", Svc_Ls_Task, NULL,
                        SVC_LS_STACK_WORDS, SVC_LS_PRIO, NULL) != OSAL_OK) {
        LOG_E("ls task creation failed\r\n");
        return SVC_ERR_HW;
    }

    /* 制动控制任务: 五路同步 + 上报 */
    if (Osal_TaskCreate("brkctl", BrkCtl_Task, NULL,
                        BRKCTL_TASK_STACK_WORDS, BRKCTL_TASK_PRIO, NULL) != OSAL_OK) {
        LOG_E("brake control task creation failed\r\n");
        return SVC_ERR_HW;
    }

    return SVC_OK;
}

void BrakeCtl_App_ForceOff(void)
{
    BrkCtl_AllOff();
    LOG_I("forced brake OFF (5 bridges off, modulation off)\r\n");
}

Svc_StatusTypeDef BrakeCtl_App_GetSnapshot(BrkCtl_SnapshotTypeDef *Out)
{
    if (Out == NULL) {
        return SVC_ERR_PARAM;
    }

    Out->Init       = gInit;
    Out->Armed      = gArmed;
    Out->On         = gOn;
    Out->Fault      = gFault;
    Out->DutyPercent = gDutyPercent;
    Out->DeadNs     = gDeadNs;
    Out->LsMask     = gLsMask;
    Out->Reports    = gReports;
    Out->DutyFrames = gDutyFrames;
    Out->DeadFrames = gDeadFrames;

    return SVC_OK;
}

/*
 * 旧实现(app/brake_app.c)已不在编译列表里: 那是"光幕逐路触发、五桥分时投入"的时序
 * 状态机, 本方案改成"上位机给占空比、五桥同步"之后不再使用。文件本体保留在仓库,
 * 需要时把它加回 UserConfig.cmake 即可复用(注意它和本模块会抢同一个功率级)。
 */
