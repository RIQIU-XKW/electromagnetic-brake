/**
 *******************************************************************************
 * @file    service_led.c
 * @brief   led 业务组件 —— led 任务 + 命令队列 + 状态快照
 *
 * 本组件只管"点灯的能力": 谁调 Svc_Led_Request*(), 就往队列里压一条命令, 由 led
 * 任务去写 breath_led IP; 状态(mode / freq / mask)统一从这里读快照。
 *
 * **命令不在本文件里**(2026-10-16 重构): 原来这里有一张常量表 Svc_Led_Commands
 * (LED_STA_0..3 精确 + LEDxxx 兜底), 由 app 层注册进 net; 现在命令全部集中到应用层:
 *
 *   app/inter_app.c    Inter_App_Commands —— 统一命令地图(哪条命令归谁)
 *   app/monitor_app.c  LED_STA_0..3 + LEDSTAT/LEDMASK/LEDON/LEDOFF/LEDFREQ 的处理
 *
 * 这样分层才干净: 组件提供能力, 认不认命令、回什么话属于业务。上面那两条命令只用
 * 本组件头文件里的 Svc_Led_* 接口, 不碰 bsp。
 *******************************************************************************
 */

#define LOG_ENABLE 0   /* 本模块日志默认关闭; 需要时改成 1 */
#define LOG_TAG "led"
#include "service/monitor_module/log/log.h"

#include "service/monitor_module/led/service_led.h"

#include "bsp/led/led.h"        /* 本组件直接驱动 breath_led IP(service -> bsp) */

#include "osal/osal.h"

/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

static Led_HandleTypeDef       gLed;
static Osal_QueueHandleTypeDef gQLed;
static uint8_t                 gReady;

static volatile uint8_t  gMode;             /**< 当前模式(快照)               */
static volatile uint8_t  gMask;             /**< 当前四路掩码(快照)           */
static volatile uint16_t gFreq;             /**< 当前频率(快照)               */
static volatile uint32_t gCommands;         /**< 收到过多少条点灯命令         */

/* ========================================================================== */
/*  内部函数(static)                                                           */
/* ========================================================================== */

/** @brief  真正去写 breath_led IP, 并更新快照. */
static void Svc_Led_Apply(const Svc_Led_MsgTypeDef *Msg)
{
    Led_StatusTypeDef status;
    Led_ModeTypeDef   mode = LED_MODE_OFF;
    uint16_t          freq = 0U;

    switch (Msg->Op) {
        case SVC_LED_OP_FREQ:
            status = Led_SetFreq(&gLed, Msg->Freq);
            break;

        case SVC_LED_OP_MASK:
            status = Led_SetMask(&gLed, Msg->Mask);
            break;

        case SVC_LED_OP_ENABLE:
            status = Led_SetEnable(&gLed, Msg->Index, Msg->On);
            break;

        case SVC_LED_OP_MODE:
        default:
            if (Msg->Freq != 0U) {
                status = Led_SetFreq(&gLed, Msg->Freq);
            } else {
                uint8_t m = Msg->Mode;

                if (m >= (uint8_t)LED_MODE_COUNT) {
                    m = (uint8_t)LED_MODE_OFF;  /* 越界的模式按关灯处理       */
                }
                status = Led_SetMode(&gLed, (Led_ModeTypeDef)m);
            }
            break;
    }

    if (status != LED_OK) {
        LOG_W("apply failed (op %u, mode %u, freq %u, idx %u, mask 0x%02X)\r\n",
              (unsigned)Msg->Op, (unsigned)Msg->Mode, (unsigned)Msg->Freq,
              (unsigned)Msg->Index, (unsigned)Msg->Mask);
        return;
    }

    /* 快照统一从驱动读回: 不管走哪条分支都只读一次, 免得每个 case 各写一遍
     * (也保证快照就是驱动眼里的状态)。 */
    (void)Led_GetState(&gLed, &mode, &freq, NULL);

    gMode = (uint8_t)mode;
    gFreq = freq;
    gMask = Led_GetMask(&gLed);
    gCommands++;

    LOG_I("op %u -> mode %s, %u Hz, mask 0x%02X\r\n",
          (unsigned)Msg->Op, Led_ModeName(mode), (unsigned)freq, (unsigned)gMask);
}

/** @brief  led 任务: 阻塞等队列里的点灯命令(创建由应用层负责, 见 service_led.h). */
void Svc_Led_Task(void *Arg)
{
    Svc_Led_MsgTypeDef msg;

    (void)Arg;

    LOG_I("task started\r\n");

    for (;;) {
        if (Osal_QueueReceive(gQLed, &msg, OSAL_WAIT_FOREVER) == OSAL_OK) {
            Svc_Led_Apply(&msg);
        }
    }
}

/* ========================================================================== */
/*  对外接口                                                                */
/* ========================================================================== */

Svc_StatusTypeDef Svc_Led_Init(void)
{
    Led_ModeTypeDef mode = LED_MODE_OFF;
    uint16_t        freq = 0U;

    if (Led_Init(&gLed, 0U) != LED_OK) {
        return SVC_ERR_HW;
    }

    gQLed = Osal_QueueCreate(SVC_LED_Q_LEN, sizeof(Svc_Led_MsgTypeDef));
    if (gQLed == NULL) {
        LOG_E("command queue creation failed\r\n");
        return SVC_ERR_HW;
    }

    (void)Led_GetState(&gLed, &mode, &freq, NULL);
    gMode = (uint8_t)mode;
    gFreq = freq;
    gMask = Led_GetMask(&gLed);         /* Led_Init 之后是 0(四路全灭)         */
    gReady = 1U;

    return SVC_OK;
}

uint8_t Svc_Led_IsReady(void)
{
    return gReady;
}

/** @brief  把一条命令压进 led 队列(四个 Request* 共用的收尾动作)。 */
static Svc_StatusTypeDef Svc_Led_Push(const Svc_Led_MsgTypeDef *Msg)
{
    if (gQLed == NULL) {
        return SVC_ERR_STATE;
    }

    if (Osal_QueueSend(gQLed, Msg, OSAL_WAIT_NOWAIT) != OSAL_OK) {
        return SVC_ERR_FULL;
    }

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Led_Request(uint8_t Mode, uint16_t Freq)
{
    /* 结构体是按值压进队列的, 所以每条命令都要用 = { 0 } 整体初始化: 局部变量里
     * 没被赋值的字段是垃圾值, 而 led 任务只看 Op 对应的字段, 一旦哪天新加字段
     * 忘了在这里填, 读到垃圾值就是一个很难查的偶发 bug。 */
    Svc_Led_MsgTypeDef msg = { 0 };

    msg.Op   = SVC_LED_OP_MODE;
    msg.Mode = Mode;
    msg.Freq = Freq;

    return Svc_Led_Push(&msg);
}

Svc_StatusTypeDef Svc_Led_RequestMask(uint8_t Mask)
{
    Svc_Led_MsgTypeDef msg = { 0 };

    msg.Op   = SVC_LED_OP_MASK;
    msg.Mask = (uint8_t)(Mask & (uint8_t)LED_MASK_ALL);

    return Svc_Led_Push(&msg);
}

Svc_StatusTypeDef Svc_Led_RequestEnable(uint8_t Index, uint8_t On)
{
    Svc_Led_MsgTypeDef msg = { 0 };

    if (Index >= (uint8_t)SVC_LED_COUNT) {
        return SVC_ERR_PARAM;
    }

    msg.Op    = SVC_LED_OP_ENABLE;
    msg.Index = Index;
    msg.On    = (On != 0U) ? 1U : 0U;

    return Svc_Led_Push(&msg);
}

Svc_StatusTypeDef Svc_Led_RequestFreq(uint16_t Freq)
{
    Svc_Led_MsgTypeDef msg = { 0 };

    if (Freq == 0U) {
        return SVC_ERR_PARAM;
    }

    msg.Op   = SVC_LED_OP_FREQ;
    msg.Freq = Freq;

    return Svc_Led_Push(&msg);
}

Svc_StatusTypeDef Svc_Led_GetSnapshot(Svc_Led_SnapshotTypeDef *Out)
{
    if (Out == NULL) {
        return SVC_ERR_PARAM;
    }

    Out->Mode     = (uint8_t)gMode;
    Out->Mask     = (uint8_t)gMask;
    Out->Freq     = (uint16_t)gFreq;
    Out->Commands = gCommands;

    return SVC_OK;
}

const char *Svc_Led_ModeName(uint8_t Mode)
{
    return Led_ModeName((Led_ModeTypeDef)Mode);
}

uint16_t Svc_Led_ModeFreq(uint8_t Mode)
{
    return Led_ModeToFreq((Led_ModeTypeDef)Mode);
}
