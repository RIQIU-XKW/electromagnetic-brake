/**
 *******************************************************************************
 * @file    Mover_app.c
 * @brief   发射小车: 单字节命令(0xD0..0xD6) + 整周期时序(非阻塞, 由本模块任务推进)
 *
 *   app/Mover_app.c        命令 / 时序状态机 / 推进任务
 *   service/Mover_module   service_motor(帧+CiA402) / service_rs485(总线) / modbus_rtu
 *   bsp/rs485
 *
 * 每一步只发一帧并记下"下一步什么时候到期", 等待全在本模块的任务里 —— 6 秒周期内
 * net 任务照常收发命令、遥测不丢。
 *******************************************************************************
 */

#define LOG_ENABLE 1   /* 时序每一步 + 命令解析各打一行 */
#define LOG_TAG "mover"
#include "service/monitor_module/log/log.h"

#include "app/Mover_app.h"
#include "app/app_config.h"                         /* APP_ENABLE_RS485 等开关 */

#include "service/Mover_module/service_motor.h"     /* 帧 + CiA402 动作 */
#include "service/Mover_module/service_rs485.h"     /* 总线任务(创建在本文件) */
#include "service/inter_module/net_text.h"          /* 拼回话文本用 */

#include "osal/osal.h"

/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

static uint8_t gGear = MOVER_GEAR_DEFAULT;      /**< 当前档位(0 = SS1 .. 5 = SS6)  */

static volatile Mover_SeqTypeDef gSeqStep;      /**< 时序走到哪一步               */
static uint32_t gSeqDueMs;                      /**< 下一步到期的 Osal_UptimeMs()  */
static int32_t  gSeqMmps;                       /**< 本次发射速度(mm/s)           */
static uint8_t  gSeqFault;                      /**< 1 = 某步发帧失败, 序列已中止  */
static uint32_t gSeqElapsedMs;                  /**< 上个整周期耗时(日志用)        */
static uint32_t gSeqStartMs;                    /**< 本周期起点                    */

static Osal_TaskHandleTypeDef gTask;
static uint8_t                gTaskReady;       /**< 任务起来了没                  */

/* ========================================================================== */
/*  档位 / 状态名                                                              */
/* ========================================================================== */

/** @brief  档位 -> 速度(mm/s); 越界按第一档处理。 */
static int32_t Mover_App_GearMmps(uint8_t Gear)
{
    if (Gear >= MOVER_GEAR_COUNT) {
        Gear = 0U;
    }

    return MOVER_GEAR_FIRST_MMPS + ((int32_t)Gear * MOVER_GEAR_STEP_MMPS);
}

const char *Mover_App_StateName(Mover_SeqTypeDef State)
{
    switch (State) {
    case MOVER_SEQ_IDLE:              return "idle";
    case MOVER_SEQ_ARM:               return "arm";
    case MOVER_SEQ_INIT_WAIT:         return "init-wait";
    case MOVER_SEQ_ENABLE_WAIT:       return "enable-wait";
    case MOVER_SEQ_SPEED_WAIT:        return "speed-wait";
    case MOVER_SEQ_OUT_GO_WAIT:       return "out-go-wait";
    case MOVER_SEQ_OUT_RUN:           return "out-run";
    case MOVER_SEQ_BACK_ENABLE_WAIT:  return "back-enable-wait";
    case MOVER_SEQ_BACK_POS_WAIT:     return "back-pos-wait";
    case MOVER_SEQ_BACK_RUN:          return "back-run";
    case MOVER_SEQ_DONE:              return "done";
    default:                          return "?";
    }
}

Mover_SeqTypeDef Mover_App_GetState(void)
{
    return gSeqStep;
}

/* ========================================================================== */
/*  时序推进(非阻塞: 每拍只做"当前该做的一件事")                                 */
/* ========================================================================== */

/**
 * @brief  执行当前步要发的帧, 再把状态推到下一步并定下到期时刻。
 *
 * 发帧失败就把序列停掉(置 IDLE + gSeqFault), 不带错状态继续往下发。
 */
static void Mover_App_SeqRun(void)
{
    const Mover_SeqTypeDef step = gSeqStep;    /* 执行期间别人可能已把它清掉 */
    Mover_SeqTypeDef next = MOVER_SEQ_IDLE;
    uint32_t delayMs = 0U;
    int ok = 1;

    LOG_I("seq step %u (%s)\r\n", (unsigned)step, Mover_App_StateName(step));

    switch (step) {
    case MOVER_SEQ_ARM:
        /* 每次发射都重新初始化, 从同一基线起步 */
        ok = Svc_Motor_DoInit();
        next = MOVER_SEQ_INIT_WAIT;
        delayMs = MOVER_INIT_SETTLE_MS;
        break;

    case MOVER_SEQ_INIT_WAIT:
        ok = Svc_Motor_DoEnable();
        next = MOVER_SEQ_ENABLE_WAIT;
        delayMs = MOVER_ENABLE_SETTLE_MS;
        break;

    case MOVER_SEQ_ENABLE_WAIT:
        ok = Svc_Motor_DoSpeed(gSeqMmps);
        next = MOVER_SEQ_SPEED_WAIT;
        delayMs = MOVER_SPEED_SETTLE_MS;
        break;

    case MOVER_SEQ_SPEED_WAIT:
        ok = Svc_Motor_DoPosition(MOVER_LAUNCH_POS);
        next = MOVER_SEQ_OUT_GO_WAIT;
        delayMs = MOVER_SPEED_SETTLE_MS;
        break;

    case MOVER_SEQ_OUT_GO_WAIT:
        ok = Svc_Motor_DoGo();
        next = MOVER_SEQ_OUT_RUN;
        delayMs = MOVER_LAUNCH_RUN_MS;
        break;

    case MOVER_SEQ_OUT_RUN:
        /* 回程: CiA402 新设定点 bit4 要先清(0x000F) 再置(0x001F) */
        ok = Svc_Motor_DoEnable();
        next = MOVER_SEQ_BACK_ENABLE_WAIT;
        delayMs = MOVER_ENABLE_SETTLE_MS;
        break;

    case MOVER_SEQ_BACK_ENABLE_WAIT:
        ok = Svc_Motor_DoPosition(0);
        next = MOVER_SEQ_BACK_POS_WAIT;
        delayMs = MOVER_SPEED_SETTLE_MS;
        break;

    case MOVER_SEQ_BACK_POS_WAIT:
        ok = Svc_Motor_DoGo();
        next = MOVER_SEQ_BACK_RUN;
        delayMs = MOVER_RETURN_MS;
        break;

    case MOVER_SEQ_BACK_RUN:
        (void)Svc_Motor_DoStop();
        gSeqElapsedMs = Osal_UptimeMs() - gSeqStartMs;
        next = MOVER_SEQ_DONE;
        break;

    default:
        next = MOVER_SEQ_IDLE;
        break;
    }

    /* 这一步执行期间序列可能已被重置, 那就不许把状态推回去 */
    if (gSeqStep != step) {
        return;
    }

    if (ok == 0) {
        gSeqFault = 1U;
        gSeqStep  = MOVER_SEQ_IDLE;

        LOG_E("seq ABORTED: frame send failed at step %u (%s)\r\n",
              (unsigned)step, Mover_App_StateName(step));
        return;
    }

    gSeqStep = next;

    if (next == MOVER_SEQ_DONE) {
        LOG_I("seq DONE in %u ms (car back at origin)\r\n", (unsigned)gSeqElapsedMs);
    }

    if ((next != MOVER_SEQ_IDLE) && (next != MOVER_SEQ_DONE)) {
        gSeqDueMs = Osal_UptimeMs() + delayMs;
    }
}

/** @brief  把推进任务叫醒(挂上新序列时用)。 */
static void Mover_App_SeqKick(void)
{
    gSeqDueMs = Osal_UptimeMs();        /* 立刻到期: 下一步马上执行 */

    if (gTask != NULL) {
        Osal_TaskNotifyGive(gTask);
    }
}

/**
 * @brief  推进任务: 按 gSeqDueMs 算出还差多少毫秒再睡, 醒来只做一次 SeqRun()。
 *
 * 没在跑序列时睡到 OSAL_WAIT_FOREVER(不占 CPU、不参与轮询)。
 */
static void Mover_App_Task(void *Arg)
{
    (void)Arg;

    LOG_I("task started\r\n");

    for (;;) {
        uint32_t waitMs = OSAL_WAIT_FOREVER;

        if ((gSeqStep != MOVER_SEQ_IDLE) && (gSeqStep != MOVER_SEQ_DONE)) {
            const int32_t remain = (int32_t)(gSeqDueMs - Osal_UptimeMs());
            waitMs = (remain <= 0) ? 0U : (uint32_t)remain;
        }

        (void)Osal_TaskNotifyTake(waitMs);

        if ((gSeqStep == MOVER_SEQ_IDLE) || (gSeqStep == MOVER_SEQ_DONE)) {
            continue;
        }
        if ((int32_t)(Osal_UptimeMs() - gSeqDueMs) >= 0) {
            Mover_App_SeqRun();
        }
    }
}

Svc_StatusTypeDef Mover_App_Start(void)
{
    /* RS-485 任务是发帧的物理通道, 和本任务在同一处起 */
#if (APP_ENABLE_RS485 != 0U)
    if (Svc_Rs485_IsReady() != 0U) {
        if (Osal_TaskCreate("rs485", Svc_Rs485_Task, NULL,
                            SVC_RS485_STACK_WORDS, SVC_RS485_PRIO, NULL) != OSAL_OK) {
            LOG_E("rs485 task creation failed\r\n");
            return SVC_ERR_HW;
        }
    } else {
        LOG_W("rs485 component not ready - task skipped\r\n");
    }
#endif

    if (gTaskReady != 0U) {
        return SVC_OK;              /* 起过了, 幂等 */
    }

    if (Osal_TaskCreate("mover", Mover_App_Task, NULL,
                        MOVER_TASK_STACK_WORDS, MOVER_TASK_PRIO, &gTask) != OSAL_OK) {
        LOG_E("task creation failed\r\n");
        return SVC_ERR_HW;
    }

    gTaskReady = 1U;

    return SVC_OK;
}

Svc_StatusTypeDef Mover_App_Launch(int32_t Mmps)
{
    if ((Mmps < MOVER_GEAR_FIRST_MMPS) ||
        (Mmps > (MOVER_GEAR_FIRST_MMPS + (int32_t)((MOVER_GEAR_COUNT - 1U)) * MOVER_GEAR_STEP_MMPS))) {
        return SVC_ERR_PARAM;
    }

    if ((gSeqStep != MOVER_SEQ_IDLE) && (gSeqStep != MOVER_SEQ_DONE)) {
        return SVC_ERR_STATE;       /* 还在跑: 不打断 */
    }
    if (gTaskReady == 0U) {
        return SVC_ERR_STATE;       /* 没有推进者, 不假装成功 */
    }

    gSeqMmps      = Mmps;
    gSeqFault     = 0U;
    gSeqElapsedMs = 0U;
    gSeqStartMs   = Osal_UptimeMs();
    gSeqStep      = MOVER_SEQ_ARM;

    Mover_App_SeqKick();

    return SVC_OK;
}

/* ========================================================================== */
/*  命令处理: 0xD0..0xD6(单字节)                                                */
/* ========================================================================== */

/**
 * @brief  处理 0xD0..0xD6(由 inter_app 的命令地图调用)。
 *
 * 0xD0 = 用当前档位发射; 0xD1..0xD6 = 选档(并记住)后立刻发射。
 * 逐字节扫整个报文, 处理一个就返回 1(一个报文只发一次); 没有本模块的字节返回 0。
 * 回话: "mover 0xD3 launch 700 mm/s armed / BUSY / ERR"。
 */
int Mover_App_Command(void *Arg, const uint8_t *Data, uint16_t Len,
                             Svc_Net_ReplyTypeDef *R)
{
    char     text[SVC_NET_MSG_MAX];
    uint16_t i;

    (void)Arg;

    if ((Data == NULL) || (Len == 0U)) {
        return 0;
    }

    for (i = 0U; i < Len; i++) {
        const uint8_t     code = Data[i];
        Svc_StatusTypeDef rc;
        int32_t           mmps;
        uint16_t          pos = 0U;

        if ((code < MOVER_CMD_LAUNCH) || (code > MOVER_CMD_GEAR_6)) {
            continue;                       /* 不是本模块的字节 */
        }

        if (code != MOVER_CMD_LAUNCH) {
            gGear = (uint8_t)(code - MOVER_CMD_GEAR_1);
        }

        mmps = Mover_App_GearMmps(gGear);
        rc   = Mover_App_Launch(mmps);

        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "mover ");
        pos = Svc_Text_AppendHex2(text, pos, (uint16_t)sizeof(text), code);
        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " launch ");
        pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), (uint32_t)mmps);
        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " mm/s ");

        if (rc == SVC_OK) {
            pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                                     "armed (cycle ~5.5 s, non-blocking)\r\n");
        } else if (rc == SVC_ERR_STATE) {
            pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                                     "BUSY (previous cycle still running)\r\n");
        } else {
            pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                                     "ERR (frame not sent)\r\n");
        }

        LOG_I("0x%02X gear %u -> %d mm/s: %s\r\n",
              (unsigned)code, (unsigned)(gGear + 1U), (int)mmps, Mover_App_StateName(gSeqStep));

        (void)Svc_Net_Reply(R, text, pos);

        return 1;               /* 认领了(一个报文只发一次) */
    }

    return 0;                   /* 没本模块的字节 */
}
