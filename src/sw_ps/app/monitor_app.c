/**
 *******************************************************************************
 * @file    monitor_app.c
 * @brief   呼吸灯命令(0xF0..0xFF)的处理 + led/mon 两条任务的创建
 *
 *   0xF0 全灭 / 0xF1..0xF3 慢·标准·快 / 0xF4 查状态 / 0xF5 全亮 /
 *   0xF6..0xF9 点亮 LED0..3 / 0xFA..0xFD 熄灭 LED0..3 /
 *   0xFE <频率/10> / 0xFF <掩码>
 *
 * 命令扫整条报文; 0xFE/0xFF 会吃掉后面那个参数字节(参数必须在同一报文里)。
 * 回话文本沿用老格式("led " 开头 + "mask=0x.." / "freq N"), 上位机按这两个模式
 * 回填界面, 改字会让界面同步失效。
 *******************************************************************************
 */

#define LOG_ENABLE 0   /* 本模块日志默认关闭; 需要时改成 1 */
#define LOG_TAG "monitor"
#include "service/monitor_module/log/log.h"

#include "app/monitor_app.h"

#include "service/monitor_module/led/service_led.h"
#include "service/monitor_module/service_monitor.h"  /* 状态巡检任务(创建在本文件) */
#include "service/inter_module/net_text.h"           /* Svc_Text_Append* */

#include "osal/osal.h"

/* ========================================================================== */
/*  任务函数                                                                     */
/* ========================================================================== */

Svc_StatusTypeDef Monitor_App_Start(void)
{
    if (Svc_Led_IsReady() == 0U) {
        LOG_E("led component not ready - task not created\r\n");
        return SVC_ERR_STATE;
    }

    if (Osal_TaskCreate("led", Svc_Led_Task, NULL,
                        SVC_LED_STACK_WORDS, SVC_LED_PRIO, NULL) != OSAL_OK) {
        LOG_E("led task creation failed\r\n");
        return SVC_ERR_HW;
    }

    /* 状态巡检任务: 参数已在 App_Init 里由 Svc_Mon_Init 存好 */
    if (Osal_TaskCreate("mon", Svc_Mon_Task, NULL,
                        SVC_MON_STACK_WORDS, SVC_MON_PRIO, NULL) != OSAL_OK) {
        LOG_E("monitor task creation failed\r\n");
        return SVC_ERR_HW;
    }

    return SVC_OK;
}

/* ========================================================================== */
/*  回话文本                                                                     */
/* ========================================================================== */

/** @brief  把掩码拼成 "0,2"(空掩码拼 "none")。 @return 新的写入位置。 */
static uint16_t Monitor_App_AppendOnList(char *dst, uint16_t pos, uint16_t size,
                                         uint8_t mask)
{
    uint8_t i;
    uint8_t first = 1U;

    if (mask == 0U) {
        return Svc_Text_AppendStr(dst, pos, size, "none");
    }

    for (i = 0U; i < (uint8_t)SVC_LED_COUNT; i++) {
        if ((mask & (uint8_t)SVC_LED_MASK_OF(i)) != 0U) {
            if (first == 0U) {
                pos = Svc_Text_AppendStr(dst, pos, size, ",");
            }
            pos   = Svc_Text_AppendUint(dst, pos, size, (uint32_t)i);
            first = 0U;
        }
    }

    return pos;
}

/** @brief  "led mask -> 0x05 (on=0,2)"。 */
static void Monitor_App_ReplyMask(Svc_Net_ReplyTypeDef *R, uint8_t Mask)
{
    char     text[SVC_NET_MSG_MAX];
    uint16_t pos;

    pos = Svc_Text_AppendStr(text, 0U, (uint16_t)sizeof(text), "led mask -> 0x");
    pos = Svc_Text_AppendHex2(text, pos, (uint16_t)sizeof(text), Mask);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " (on=");
    pos = Monitor_App_AppendOnList(text, pos, (uint16_t)sizeof(text), Mask);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), ")\r\n");

    (void)Svc_Net_Reply(R, text, pos);
}

/** @brief  "led freq -> 200 Hz (shared by all four LEDs, mask unchanged)"。 */
static void Monitor_App_ReplyFreq(Svc_Net_ReplyTypeDef *R, uint16_t Hz)
{
    char     text[SVC_NET_MSG_MAX];
    uint16_t pos;

    pos = Svc_Text_AppendStr(text, 0U, (uint16_t)sizeof(text), "led freq -> ");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), (uint32_t)Hz);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                             " Hz (shared by all four LEDs, mask unchanged)\r\n");

    (void)Svc_Net_Reply(R, text, pos);
}

/** @brief  "led state -> normal (0xF2), freq 100 Hz"(括号里回显能直接发的字节)。 */
static void Monitor_App_ReplyState(Svc_Net_ReplyTypeDef *R, uint8_t State)
{
    char     text[SVC_NET_MSG_MAX];
    uint16_t pos;

    pos = Svc_Text_AppendStr(text, 0U, (uint16_t)sizeof(text), "led state -> ");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), Svc_Led_ModeName(State));
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " (0x");
    pos = Svc_Text_AppendHex2(text, pos, (uint16_t)sizeof(text),
                              (uint8_t)(LED_CMD_ALL_OFF + State));
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "), freq ");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text),
                              (uint32_t)Svc_Led_ModeFreq(State));
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " Hz\r\n");

    (void)Svc_Net_Reply(R, text, pos);
}

/** @brief  0xF4: "led mask=0x05 on=0,2 freq=100Hz state=normal"。 */
static void Monitor_App_ReplyStatus(Svc_Net_ReplyTypeDef *R)
{
    Svc_Led_SnapshotTypeDef snap;
    char                    text[SVC_NET_MSG_MAX];
    uint16_t                pos;

    (void)Svc_Led_GetSnapshot(&snap);

    pos = Svc_Text_AppendStr(text, 0U, (uint16_t)sizeof(text), "led mask=0x");
    pos = Svc_Text_AppendHex2(text, pos, (uint16_t)sizeof(text), snap.Mask);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " on=");
    pos = Monitor_App_AppendOnList(text, pos, (uint16_t)sizeof(text), snap.Mask);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " freq=");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), (uint32_t)snap.Freq);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "Hz state=");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), Svc_Led_ModeName(snap.Mode));
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "\r\n");

    (void)Svc_Net_Reply(R, text, pos);
}

/** @brief  用法提示, 以 "led: " 开头。 */
static void Monitor_App_ReplyUsage(Svc_Net_ReplyTypeDef *R, const char *What)
{
    char     text[SVC_NET_MSG_MAX];
    uint16_t pos;

    pos = Svc_Text_AppendStr(text, 0U, (uint16_t)sizeof(text), "led: ");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), What);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "\r\n");

    (void)Svc_Net_Reply(R, text, pos);
}

/* ========================================================================== */
/*  单条命令的动作                                                             */
/* ========================================================================== */

/** @brief  0xF0..0xF3: 状态 0=关灯 / 1=慢 / 2=标准 / 3=快。 */
static void Monitor_App_DoState(Svc_Net_ReplyTypeDef *R, uint8_t State)
{
    (void)Svc_Led_Request(State, 0U);
    Monitor_App_ReplyState(R, State);
}

/** @brief  设四路掩码(0xF5 全亮 / 0xFF 带参数)。 */
static void Monitor_App_DoMask(Svc_Net_ReplyTypeDef *R, uint8_t Mask)
{
    (void)Svc_Led_RequestMask(Mask);
    Monitor_App_ReplyMask(R, Mask);
}

/**
 * @brief  0xF6..0xFD: 单独点亮/熄灭一路。
 *
 * 回话按"预期结果"算: 命令是压进队列异步执行的, 所以拿快照当基准再把这一位改掉。
 */
static void Monitor_App_DoOnOff(Svc_Net_ReplyTypeDef *R, uint8_t Index, uint8_t On)
{
    Svc_Led_SnapshotTypeDef snap;
    char                    text[SVC_NET_MSG_MAX];
    uint16_t                pos;
    uint8_t                 mask;

    (void)Svc_Led_RequestEnable(Index, On);
    (void)Svc_Led_GetSnapshot(&snap);

    mask = snap.Mask;

    if (On != 0U) {
        mask = (uint8_t)(mask | SVC_LED_MASK_OF(Index));
    } else {
        mask = (uint8_t)(mask & (uint8_t)~SVC_LED_MASK_OF(Index));
    }

    pos = Svc_Text_AppendStr(text, 0U, (uint16_t)sizeof(text), "led ");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), (uint32_t)Index);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                             (On != 0U) ? " ON, mask=0x" : " OFF, mask=0x");
    pos = Svc_Text_AppendHex2(text, pos, (uint16_t)sizeof(text), mask);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " (on=");
    pos = Monitor_App_AppendOnList(text, pos, (uint16_t)sizeof(text), mask);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), ")\r\n");

    (void)Svc_Net_Reply(R, text, pos);
}

/** @brief  0xFE <参数>: 设共享频率(参数 = Hz/10, 合法 1..100)。 */
static void Monitor_App_DoFreq(Svc_Net_ReplyTypeDef *R, uint8_t Code)
{
    /* 上下界都查: 越界就回提示, 不能收下再报一个板子上不存在的频率 */
    const uint16_t hz = (uint16_t)((uint16_t)Code * (uint16_t)LED_CMD_FREQ_UNIT);

    if ((hz < (uint16_t)SVC_LED_FREQ_MIN) || (hz > (uint16_t)SVC_LED_FREQ_MAX)) {
        Monitor_App_ReplyUsage(R,
            "0xFE <freq/10> out of range (1..100 = 10..1000 Hz)");
        return;
    }

    (void)Svc_Led_RequestFreq(hz);
    Monitor_App_ReplyFreq(R, hz);
}

/* ========================================================================== */
/*  兜底处理函数                                                               */
/* ========================================================================== */

/** @brief  0xFE / 0xFF 的参数没跟上来时的提示。 */
static void Monitor_App_ReplyMissingArg(Svc_Net_ReplyTypeDef *R, uint8_t Cmd)
{
    char     text[SVC_NET_MSG_MAX];
    uint16_t pos;

    pos = Svc_Text_AppendStr(text, 0U, (uint16_t)sizeof(text), "led: 0x");
    pos = Svc_Text_AppendHex2(text, pos, (uint16_t)sizeof(text), Cmd);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                             " needs its parameter byte in the SAME tcp packet "
                             "(0xFE <freq/10> | 0xFF <mask>)\r\n");

    (void)Svc_Net_Reply(R, text, pos);
}

/**
 * @brief  处理 0xF0..0xFF(由 inter_app 的命令地图调用)。
 *
 * 扫整条报文; 一个 0xF? 都没有时返回 0 放行给别的兜底项(文本命令都是 ASCII,
 * 别的模块的字节段也不重叠)。
 */
int Monitor_App_Command(void *Arg, const uint8_t *Data, uint16_t Len,
                        Svc_Net_ReplyTypeDef *R)
{
    uint16_t i;
    uint8_t  claimed = 0U;

    (void)Arg;

    if ((Data == NULL) || (Len == 0U)) {
        return 0;
    }

    for (i = 0U; i < Len; i++) {
        const uint8_t cmd = Data[i];

        if (cmd < (uint8_t)LED_CMD_ALL_OFF) {
            continue;                       /* 不是呼吸灯命名空间 */
        }

        claimed = 1U;

        if (cmd <= (uint8_t)LED_CMD_FAST) {                 /* 0xF0..0xF3 */
            Monitor_App_DoState(R, (uint8_t)(cmd - (uint8_t)LED_CMD_ALL_OFF));
            continue;
        }

        if (cmd == (uint8_t)LED_CMD_STATUS) {                /* 0xF4 */
            Monitor_App_ReplyStatus(R);
            continue;
        }

        if (cmd == (uint8_t)LED_CMD_ALL_ON) {                /* 0xF5 */
            Monitor_App_DoMask(R, (uint8_t)SVC_LED_MASK_ALL);
            continue;
        }

        if ((cmd >= (uint8_t)LED_CMD_ON_BASE) && (cmd <= (uint8_t)(LED_CMD_ON_BASE + 3U))) {
            Monitor_App_DoOnOff(R, (uint8_t)(cmd - (uint8_t)LED_CMD_ON_BASE), 1U);
            continue;
        }

        if ((cmd >= (uint8_t)LED_CMD_OFF_BASE) && (cmd <= (uint8_t)(LED_CMD_OFF_BASE + 3U))) {
            Monitor_App_DoOnOff(R, (uint8_t)(cmd - (uint8_t)LED_CMD_OFF_BASE), 0U);
            continue;
        }

        if (cmd == (uint8_t)LED_CMD_FREQ) {                  /* 0xFE <频率/10> */
            if ((uint16_t)(i + 1U) >= Len) {
                Monitor_App_ReplyMissingArg(R, cmd);
                break;                      /* 参数没来, 后面的字节也解释不了 */
            }

            i++;
            Monitor_App_DoFreq(R, Data[i]);
            continue;
        }

        /* 剩下只有 0xFF <掩码> */
        if ((uint16_t)(i + 1U) >= Len) {
            Monitor_App_ReplyMissingArg(R, cmd);
            break;
        }

        i++;

        if (Data[i] > (uint8_t)SVC_LED_MASK_ALL) {
            Monitor_App_ReplyUsage(R, "0xFF <mask> out of range (0..15)");
            continue;
        }

        Monitor_App_DoMask(R, Data[i]);
    }

    return (claimed != 0U) ? 1 : 0;
}
