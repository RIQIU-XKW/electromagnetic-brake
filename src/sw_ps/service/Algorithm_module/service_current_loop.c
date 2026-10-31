/**
 *******************************************************************************
 * @file    service_current_loop.c
 * @brief   5 路电流负反馈闭环: 反馈读 ADS, 输出写各桥占空比
 *
 * 每 10 ms 一拍:
 *     1. 取 5 路线圈电流(A) 当反馈
 *     2. 每桥自己的 PID: err = 目标 - 实际; 上限各桥不同(37/40/28/45/75 %)
 *     3. 输出写成千分比丢给 PWM(腿 A 静态高 + 腿 B 斩波)
 *     4. 目标为 0 时闭环把占空比爬到 0, 然后关掉桥输出与全局调制
 *
 * 投入的那一刻顺带把死区设成 100 ns(上位机之后发 0xDC 帧仍可改, 闭环不会再刷回去)。
 *******************************************************************************
 */

#define LOG_ENABLE 1   /* 起停/目标变化各打一行 */
#define LOG_TAG "curloop"
#include "service/monitor_module/log/log.h"

#include "service/Algorithm_module/service_current_loop.h"

#include "service/Samp_module/service_ads.h"        /* 电流反馈 */
#include "service/brake_module/service_pwm.h"       /* 占空比输出 */
#include "osal/osal.h"

/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

static uint8_t  gReady;                                     /**< 1 = 初始化完成      */
static uint8_t  gActive;                                    /**< 1 = 正在闭环输出    */

/**
 * 外部"立刻放手"请求(1 = 有待处理的放手)。
 *
 * 为什么要这个标志而不是直接复位: Svc_CurLoop_Stop() 是从**别的任务**(net 命令任务 /
 * brkctl 任务)里调的, 在那里动 gPid[] 会和本任务正在跑的 Svc_Pid_Update() 撞上。
 * 所以外部只置个标志, 真正的复位在本任务上下文里做(见 Svc_CurLoop_Task 开头)。
 */
static volatile uint8_t gStopReq;

/** 每桥一套 PID 状态(独立实例, 绝不共用)。 */
static Svc_Pid_StateTypeDef gPid[SVC_CURLOOP_BRIDGES];

/** 各桥占空比硬上限(%) —— 初始化时从宏装进来。 */
static const float gDutyMax[SVC_CURLOOP_BRIDGES] = {
    SVC_CURLOOP_DUTY_MAX_P1,
    SVC_CURLOOP_DUTY_MAX_P2,
    SVC_CURLOOP_DUTY_MAX_P3,
    SVC_CURLOOP_DUTY_MAX_P4,
    SVC_CURLOOP_DUTY_MAX_P5,
};

static float    gTargetA;                                   /**< 5 桥共用的目标电流(A) */
static float    gActualA[SVC_CURLOOP_BRIDGES];              /**< 最近一拍的实际电流(A) */
static float    gDutyPct[SVC_CURLOOP_BRIDGES];               /**< 最近一拍的占空比(%)   */

/** 各桥上一次真正写进 PWM 的千分比(用来避免同值重复写)。 */
static uint32_t gPushedPermille[SVC_CURLOOP_BRIDGES];

/** "这个值还没写过"的哨兵。占空比千分比只可能 0..1000, 0xFFFFFFFF 撞不上。 */
#define SVC_CURLOOP_PERMILLE_NONE   0xFFFFFFFFU

static uint32_t gCycles;                                    /**< 跑过的控制周期数      */
static uint32_t gCmds;                                      /**< 收到的电流指令条数    */

/* ========================================================================== */
/*  内部函数(static)                                                           */
/* ========================================================================== */

/** @brief  把 5 路反馈读成 A(ADS 给的是 mA)。 */
static void Svc_CurLoop_ReadFeedback(void)
{
    int32_t ma[SVC_ADS_COILS];
    uint8_t i;

    if (Svc_Ads_GetCoilCurrentMa(ma, (uint32_t)SVC_ADS_COILS) != SVC_OK) {
        return;
    }

    for (i = 0U; (i < (uint8_t)SVC_CURLOOP_BRIDGES) && (i < (uint8_t)SVC_ADS_COILS); i++) {
        gActualA[i] = (float)ma[i] / 1000.0f;
    }
}

/** @brief  把某桥占空比(%)写成千分比给 PWM(值没变就不重复写)。 */
static void Svc_CurLoop_ApplyDuty(uint8_t Bridge, float Percent)
{
    uint32_t permille;

    if (Percent < 0.0f) {
        Percent = 0.0f;
    }
    if (Percent > 100.0f) {
        Percent = 100.0f;
    }

    permille = (uint32_t)((Percent * 10.0f) + 0.5f);     /* % -> 千分比, 四舍五入 */
    if (permille > 1000U) {
        permille = 1000U;
    }

    /*
     * 值没变就不写。
     *
     * 为什么: 目标为 0 以后闭环还会一直跑(每 10 ms 一拍), 如果每拍都往 PWM 里写一次
     * 0%, 就把**占空比模式**那条链路(brkctl: 0xA1 / 占空比帧)刚设好的值又抹成 0 ——
     * 两条链路抢同一个功率级。但也不能"到 0 就不写": 那样最后一档非零占空比会一直
     * 留在寄存器里。所以按"和上次写下去的值比"来去重, 退到 0 的那一拍恰好写一次 0。
     */
    if (permille == gPushedPermille[Bridge]) {
        return;
    }

    if (Svc_Pwm_SetBridgeBrakePermille((int)Bridge, permille) == SVC_OK) {
        gPushedPermille[Bridge] = permille;
    }
}

/** @brief  所有桥占空比都退到 0 了没有(用来判断能不能关输出)。 */
static uint8_t Svc_CurLoop_AllZero(void)
{
    uint8_t i;

    for (i = 0U; i < (uint8_t)SVC_CURLOOP_BRIDGES; i++) {
        if (gDutyPct[i] > 0.001f) {
            return 0U;
        }
    }

    return 1U;
}

/** @brief  闭环退出: 关桥输出 + 关全局调制(占空比已经爬到 0 之后才调)。 */
static void Svc_CurLoop_OutputOff(void)
{
    (void)Svc_Pwm_SetAllBridgeEnable(0);
    (void)Svc_Pwm_SetGlobalEnable(0);
    gActive = 0U;
}

/** @brief  闭环投入: 死区按 100 ns 设一遍 -> 开全局调制 -> 五桥同时打开。 */
static void Svc_CurLoop_OutputOn(void)
{
    uint8_t i;

    /*
     * 死区只在投入这一拍写一次(见 SVC_CURLOOP_DEAD_NS_DEFAULT 的说明):
     * 上位机在 PWM 栏发 0xDC 帧随时能改, 闭环不会每拍把它刷回去。
     */
    if (Svc_Pwm_SetAllDeadtimeNs((uint32_t)SVC_CURLOOP_DEAD_NS_DEFAULT) != SVC_OK) {
        LOG_E("apply dead time %u ns failed\r\n", (unsigned)SVC_CURLOOP_DEAD_NS_DEFAULT);
    }

    /* 上一轮的值不再代表 PWM 里的实际内容(可能被别人改过), 作废缓存, 下一拍重写 */
    for (i = 0U; i < (uint8_t)SVC_CURLOOP_BRIDGES; i++) {
        gPushedPermille[i] = SVC_CURLOOP_PERMILLE_NONE;
    }

    (void)Svc_Pwm_SetGlobalEnable(1);
    (void)Svc_Pwm_SetAllBridgeEnable(1);
    gActive = 1U;
}

/* ========================================================================== */
/*  任务函数                                                                   */
/* ========================================================================== */

void Svc_CurLoop_Task(void *Arg)
{
    const float dt = (float)SVC_CURLOOP_PERIOD_MS / 1000.0f;
    uint8_t     i;

    (void)Arg;

    for (;;) {
        /*
         * 外部要求"立刻放手"(0xA5 / 过流 / 切回占空比模式): 在这里把状态清干净。
         * 放在任务上下文里做, 就不会和下面的 Svc_Pid_Update() 抢同一份 PID 状态。
         *
         * 不碰 PWM 的使能和占空比: 那几个是调用方正在接手的东西(它已经把桥关了,
         * 或者正要写自己的占空比)。我们只要保证下一拍不会再把桥打开。
         */
        if (gStopReq != 0U) {
            gStopReq = 0U;
            gTargetA = 0.0f;
            gActive  = 0U;

            for (i = 0U; i < (uint8_t)SVC_CURLOOP_BRIDGES; i++) {
                Svc_Pid_Reset(&gPid[i]);
                gDutyPct[i]        = 0.0f;
                gPushedPermille[i] = SVC_CURLOOP_PERMILLE_NONE;
            }

            LOG_I("released PWM control (external stop)\r\n");
        }

        Svc_CurLoop_ReadFeedback();

        /* 目标为 0: 让占空比按爬坡速度退回 0, 全退干净了再关输出 */
        if (gTargetA < SVC_CURLOOP_IDLE_A) {
            for (i = 0U; i < (uint8_t)SVC_CURLOOP_BRIDGES; i++) {
                (void)Svc_Pid_Update(&gPid[i], 0.0f, gActualA[i], dt);   /* 走同一套爬坡 */
                gPid[i].Integ = 0.0f;                                    /* 停止时清积分 */
                gPid[i].PrevErr = 0.0f;
                gDutyPct[i]  = Svc_Pid_GetOutput(&gPid[i]);

                /*
                 * 只在"寄存器里还留着**我们自己**写下去的非零占空比"时才写:
                 *   · 退坡期间一直写, 把占空比一路压到 0;
                 *   · 压到 0 的那一拍刚好写一次 0 把残余清掉, 之后 gPushedPermille 变 0,
                 *     这个 if 不再成立 —— 于是不会每拍往 PWM 里灌 0 去抹掉占空比模式
                 *     (brkctl 0xA1 / 占空比帧)设好的值;
                 *   · 外部停(0xA5/过流/切回占空比)之后缓存被作废成 NONE, 这里也不写,
                 *     功率级归调用方管。
                 */
                if ((gPushedPermille[i] != SVC_CURLOOP_PERMILLE_NONE) &&
                    (gPushedPermille[i] != 0U)) {
                    Svc_CurLoop_ApplyDuty(i, gDutyPct[i]);
                }
            }

            if ((gActive != 0U) && (Svc_CurLoop_AllZero() != 0U)) {
                Svc_CurLoop_OutputOff();
                LOG_I("brake current loop OFF (target 0 A, duty ramped to 0)\r\n");
            }

            gCycles++;
            Osal_DelayMs((uint32_t)SVC_CURLOOP_PERIOD_MS);
            continue;
        }

        /* 目标 > 0: 先确保输出是开的 */
        if (gActive == 0U) {
            Svc_CurLoop_OutputOn();
            LOG_I("brake current loop ON, target %.2f A, dead time %u ns\r\n",
                  (double)gTargetA, (unsigned)SVC_CURLOOP_DEAD_NS_DEFAULT);
        }

        /* 5 路各自跑自己的 PID, 各写各的占空比 */
        for (i = 0U; i < (uint8_t)SVC_CURLOOP_BRIDGES; i++) {
            gDutyPct[i] = Svc_Pid_Update(&gPid[i], gTargetA, gActualA[i], dt);
            Svc_CurLoop_ApplyDuty(i, gDutyPct[i]);
        }

        gCycles++;

        Osal_DelayMs((uint32_t)SVC_CURLOOP_PERIOD_MS);
    }
}

/* ========================================================================== */
/*  对外接口                                                                   */
/* ========================================================================== */

Svc_StatusTypeDef Svc_CurLoop_Init(void)
{
    uint8_t i;

    for (i = 0U; i < (uint8_t)SVC_CURLOOP_BRIDGES; i++) {
        /* 每桥一套独立状态; 上限各桥不同; 爬坡步长统一 */
        Svc_Pid_InitSetup(&gPid[i],
                          SVC_CURLOOP_KP, SVC_CURLOOP_KI, SVC_CURLOOP_KD,
                          SVC_CURLOOP_DUTY_MIN, gDutyMax[i],
                          SVC_CURLOOP_SLEW_STEP_PER_CYCLE);

        gActualA[i] = 0.0f;
        gDutyPct[i] = 0.0f;
        gPushedPermille[i] = SVC_CURLOOP_PERMILLE_NONE;
    }

    gTargetA = 0.0f;
    gActive  = 0U;
    gStopReq = 0U;
    gCycles  = 0U;
    gCmds    = 0U;
    gReady   = 1U;

    LOG_I("ready: 5 independent current loops, duty limits %u/%u/%u/%u/%u %%, slew %.1f %%/cycle\r\n",
          (unsigned)gDutyMax[0], (unsigned)gDutyMax[1], (unsigned)gDutyMax[2],
          (unsigned)gDutyMax[3], (unsigned)gDutyMax[4],
          (double)SVC_CURLOOP_SLEW_STEP_PER_CYCLE);

    return SVC_OK;
}

Svc_StatusTypeDef Svc_CurLoop_SetTargetA(float Amps)
{
    if (gReady == 0U) {
        return SVC_ERR_STATE;
    }

    if (Amps < 0.0f) {
        Amps = 0.0f;
    }
    if (Amps > SVC_CURLOOP_CURRENT_MAX_A) {
        Amps = SVC_CURLOOP_CURRENT_MAX_A;
    }

    /*
     * 新的电流指令一律**作废**待处理的"外部放手"请求 —— 规矩是最后一条命令说了算:
     * 刚被 0xA5 停掉、10 ms 内又收到电流指令, 那就该按新指令跑, 不能被那一拍清掉。
     */
    gStopReq = 0U;

    gTargetA = Amps;
    gCmds++;

    return SVC_OK;
}

float Svc_CurLoop_GetTargetA(void)
{
    return gTargetA;
}

Svc_StatusTypeDef Svc_CurLoop_Stop(void)
{
    if (gReady == 0U) {
        return SVC_ERR_STATE;
    }

    /*
     * "外部接管式"停止: 立刻放手, 不做爬坡。
     *
     * 谁会调用它: 0xA5 停制动 / 过流保护 / 上位机改发占空比帧(切回占空比模式)。
     * 这几种情况下功率级马上要归别人管(或者已经被硬件关了), 这里**只**把自己的状态
     * 清干净 —— 目标 0 + 5 路 PID 复位 + 不再认为自己握着输出 —— 不去碰 PWM 的
     * 使能/全局调制, 也不写占空比:
     *   · 0xA5 / 过流: 调用方已经把桥和调制关了, 占空比是多少都不再输出;
     *   · 切回占空比模式: 调用方紧接着就写自己的占空比, 这里再插一手只会打架。
     *
     * 注意别和"滑块拖到 0"混了: 那条走 Svc_CurLoop_SetTargetA(0), 是**自己**退出,
     * 会按爬坡速度把占空比退到 0 再关桥输出(见 Svc_CurLoop_Task)。
     *
     * 实现上只有 gTargetA/gActive 这两个字在这里写(单字 32 位/8 位写, 够用),
     * PID 状态的复位交给控制任务自己去做(见 gStopReq) —— 本函数是从别的任务调的,
     * 直接动 gPid[] 会和正在跑的 Svc_Pid_Update() 撞上。
     */
    gTargetA = 0.0f;
    gActive  = 0U;
    gStopReq = 1U;

    return SVC_OK;
}

Svc_StatusTypeDef Svc_CurLoop_GetSnapshot(Svc_CurLoop_SnapshotTypeDef *Out)
{
    uint8_t i;

    if (Out == NULL) {
        return SVC_ERR_PARAM;
    }

    Out->Ready  = gReady;
    Out->Active = gActive;
    Out->TargetA = gTargetA;
    Out->Cycles = gCycles;
    Out->Cmds   = gCmds;

    for (i = 0U; i < (uint8_t)SVC_CURLOOP_BRIDGES; i++) {
        Out->ActualA[i]      = gActualA[i];
        Out->DutyPercent[i]  = gDutyPct[i];
        Out->LimitPercent[i] = gDutyMax[i];
    }

    return SVC_OK;
}
