/**
 *******************************************************************************
 * @file    inter_app.c
 * @brief   人机交互: STATUS 一行总状态 + 按键(0xAA..0xAC 命令 / [0xAA] 上报)
 *          + 全板统一命令地图(Inter_App_Commands)
 *
 * 只调 service/ 的接口, 不 include 任何 bsp 头。
 *******************************************************************************
 */

#define LOG_ENABLE 0   /* 命令回话已经足够说明问题 */
#define LOG_TAG "inter"
#include "service/monitor_module/log/log.h"

#include "app/inter_app.h"
#include "app/app_config.h"                    /* APP_ENABLE_* / APP_KEY2_RESTART_ENABLE */
#include "app/brake_ctl_app.h"                  /* BrakeCtl_App_Command + 制动快照 */
#include "app/current_app.h"                    /* Current_App_Command(电流指令帧 0xBA..0xAB) */
#include "app/monitor_app.h"                    /* Monitor_App_Command(0xF0..0xFF) */
#if (APP_ENABLE_MOTOR != 0U)
#include "app/Mover_app.h"                      /* Mover_App_Command(0xD0..0xD6) */
#endif
#if (APP_ENABLE_ADS != 0U)
#include "app/samp_app.h"                       /* Samp_App_Command(0xE0..0xE2) */
#endif

#include "service/inter_module/net_text.h"      /* Svc_Text_Append* */
#include "service/inter_module/service_key.h"   /* 按键: 快照 + 动作 + 事件回调 */
#include "service/monitor_module/led/service_led.h"
#include "service/brake_module/service_pwm.h"
#include "service/brake_module/service_oc.h"
#if (APP_ENABLE_LS != 0U)
#include "service/brake_module/service_ls.h"
#endif

#include "osal/osal.h"

#include <string.h>

/* ========================================================================== */
/*  任务(net 必须最先起: 别的组件靠它的广播队列发遥测)                          */
/* ========================================================================== */

Svc_StatusTypeDef Inter_App_Start(void)
{
    if (Svc_Net_IsReady() == 0U) {
        LOG_E("net component not ready - task not created\r\n");
        return SVC_ERR_STATE;
    }

    if (Osal_TaskCreate("net", Svc_Net_Task, NULL,
                        SVC_NET_STACK_WORDS, SVC_NET_PRIO, NULL) != OSAL_OK) {
        LOG_E("net task creation failed\r\n");
        return SVC_ERR_HW;
    }

    /* key 任务起不来不算致命(按键不能用, 别的功能照旧), 只警告 */
#if (APP_ENABLE_KEY != 0U)
    if (Svc_Key_IsReady() != 0U) {
        if (Osal_TaskCreate("key", Svc_Key_Task, NULL,
                            SVC_KEY_STACK_WORDS, SVC_KEY_PRIO, NULL) != OSAL_OK) {
            LOG_W("key task creation failed (the keys will not work)\r\n");
        }
    } else {
        LOG_W("key component not ready - task skipped\r\n");
    }
#endif

    return SVC_OK;
}

/* ========================================================================== */
/*  STATUS: 一行总状态                                                          */
/* ========================================================================== */

/** @brief  IP 来源的名字。 */
static const char *Inter_AddrSourceName(uint8_t Source)
{
    switch (Source) {
        case 1U:  return "dhcp";
        case 2U:  return "static";
        default:  return "waiting";
    }
}

/**
 * @brief  STATUS: 灯 / 网口 / PWM / 光幕 / 过流诊断各取一段拼成一行。
 *
 *   zynq | led=0x01/normal/100Hz | ip .. (static) | net=up/100M/cli=1 |
 *   pwm en=0x00000003 oc=0/0 | cmds=7 | ls=0x00 | ocEv=0x00 ocLt=0x00 ocNw=0 rpt=1
 *
 *   ocEv = 软件累计位(上电以来出现过过流的桥)  ocLt = 硬件粘滞位
 *   ocNw = 此刻引脚是否还低                    rpt  = 已发出的上报条数
 *
 * ⚠ 加字段前先数长度(最坏约 180 字节 < SVC_NET_MSG_MAX=192):
 *   Svc_Text_AppendStr 满了会静默截断, 丢掉的是结尾的 \r\n。
 */
static int Inter_StatusCommand(void *Arg, const uint8_t *Data, uint16_t Len,
                               Svc_Net_ReplyTypeDef *R)
{
    Svc_Net_SnapshotTypeDef net;
    Svc_Led_SnapshotTypeDef led;
    Svc_Pwm_SnapshotTypeDef pwm;
    Svc_Oc_SnapshotTypeDef  oc;
    BrkCtl_SnapshotTypeDef  brake;
#if (APP_ENABLE_LS != 0U)
    Svc_Ls_SnapshotTypeDef  ls;
#endif
    char                    text[SVC_NET_MSG_MAX];
    uint16_t                pos = 0U;

    (void)Arg;
    (void)Data;
    (void)Len;

    (void)Svc_Net_GetSnapshot(&net);
    (void)Svc_Led_GetSnapshot(&led);
    (void)Svc_Pwm_GetSnapshot(&pwm);
    (void)Svc_Oc_GetSnapshot(&oc);
    (void)BrakeCtl_App_GetSnapshot(&brake);
#if (APP_ENABLE_LS != 0U)
    (void)Svc_Ls_GetSnapshot(&ls);
#endif

    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "zynq | led=0x");
    pos = Svc_Text_AppendHex2(text, pos, (uint16_t)sizeof(text), led.Mask);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "/");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), Svc_Led_ModeName(led.Mode));
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "/");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), (uint32_t)led.Freq);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "Hz | ip ");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), net.Ip);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " (");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                             Inter_AddrSourceName(net.AddrSource));
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), ") | net=");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                             (net.LinkUp != 0U) ? "up" : "down");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "/");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), (uint32_t)net.SpeedMbps);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "M/cli=");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), (uint32_t)net.Clients);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " | pwm en=0x");
    pos = Svc_Text_AppendHex32(text, pos, (uint16_t)sizeof(text), pwm.EnableMask);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " oc=");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), pwm.OcNow);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "/");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), pwm.OcLatched);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " | cmds=");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), net.TcpCmds);
#if (APP_ENABLE_LS != 0U)
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " | ls=0x");
    pos = Svc_Text_AppendHex2(text, pos, (uint16_t)sizeof(text), ls.State);
#endif

    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " | ocEv=0x");
    pos = Svc_Text_AppendHex2(text, pos, (uint16_t)sizeof(text),
                              (uint8_t)(oc.EventMask & 0xFFU));
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " ocLt=0x");
    pos = Svc_Text_AppendHex2(text, pos, (uint16_t)sizeof(text),
                              (uint8_t)(oc.LatchedMask & 0xFFU));
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " ocNw=");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), (uint32_t)oc.Alarm);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " brk=");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), (brake.On != 0U) ? "on" : "off");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "/");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), brake.DutyPercent);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "%/");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), brake.DeadNs);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "ns rpt=");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), brake.Reports);

    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "\r\n");

    (void)Svc_Net_Reply(R, text, pos);

    return 1;
}

/* ========================================================================== */
/*  按键: 命令(0xAA 查状态 / 0xAB 软复位 / 0xAC 重启网口) 与上报               */
/* ========================================================================== */

#if (APP_ENABLE_KEY != 0U)

/** @brief  回一句固定文本(长度用 strlen 算)。 */
static void Inter_ReplyText(Svc_Net_ReplyTypeDef *R, const char *Text)
{
    (void)Svc_Net_Reply(R, Text, (uint16_t)strlen(Text));
}

/** @brief  0xAA: 一行按键状态(电平 / 按下次数 / 复位次数 + KEY2 动作开没开)。 */
static void Inter_KeyStatus(Svc_Net_ReplyTypeDef *R)
{
    char                    text[SVC_NET_MSG_MAX];
    uint16_t                pos = 0U;
    Svc_Key_SnapshotTypeDef snap;

    (void)Svc_Key_GetSnapshot(&snap);

    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "key ready=");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), (uint32_t)snap.Ready);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " armed=");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), (uint32_t)snap.Armed);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " | KEY1(MIO10/E9)=");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                             (snap.Key1Level == 0U) ? "DOWN" : "up");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " n=");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), snap.Key1Presses);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " | KEY2(MIO11/C6)=");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                             (snap.Key2Level == 0U) ? "DOWN" : "up");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " n=");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), snap.Key2Presses);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                             (snap.Key2Enabled != 0U) ? "" : " (action disabled)");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " | resets=");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), snap.Resets);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " netrestarts=");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), snap.NetRestarts);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "\r\n");

    (void)Svc_Net_Reply(R, text, pos);
}

/**
 * @brief  处理 0xAA..0xAC(由命令地图调用)。
 *
 * 0xAB 会真的复位, 回复多半发不出去(正常表现); 0xAC 没开时如实回 disabled。
 * 扫描整条报文, 别的字节不认领(返回 0 放行给制动/发射/采样/灯那几段)。
 */
int Inter_KeyCommand(void *Arg, const uint8_t *Data, uint16_t Len,
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

        if (cmd == (uint8_t)INTER_KEY_CMD_STATUS) {
            claimed = 1U;
            Inter_KeyStatus(R);
            continue;
        }

        if (cmd == (uint8_t)INTER_KEY_CMD_RESET) {
            claimed = 1U;

            Inter_ReplyText(R, "key: soft reset (0xAB) - the PS is restarting now\r\n");

            Svc_Key_RequestReset();     /* 正常不返回 */

            Inter_ReplyText(R, "key: soft reset did NOT take effect\r\n");
            continue;
        }

        if (cmd == (uint8_t)INTER_KEY_CMD_NET) {
            claimed = 1U;

            if (Svc_Key_RequestNetRestart() == 0) {
                Inter_ReplyText(R,
                    "key: tcp restart (0xAC) disabled (KEY2 is kept for later use; "
                    "set APP_KEY2_RESTART_ENABLE = 1 to enable)\r\n");
            } else {
                Inter_ReplyText(R, "key: restarting the tcp server (0xAC)\r\n");
            }
            continue;
        }
    }

    return (claimed != 0U) ? 1 : 0;
}

/**
 * @brief  按键状态变化 -> 广播 [0xAA][掩码](bit0 = KEY1 按着, bit1 = KEY2 按着)。
 *
 * 挂在 service_key 的事件回调上, 跑在 key 任务里 —— Svc_Net_SendBytes 只入队, 立刻返回。
 * service_key 保证"先回调再复位/重连", 所以这两字节总能先发出去。
 */
static void Inter_KeyOnEvent(uint8_t PressedMask)
{
    uint8_t frame[2];

    frame[0] = (uint8_t)INTER_KEY_RPT_STATUS;
    frame[1] = PressedMask;

    if (Svc_Net_SendBytes(frame, 2U) == SVC_OK) {
        LOG_I("key report [0x%02X 0x%02X] queued\r\n",
              (unsigned)frame[0], (unsigned)frame[1]);
    } else {
        LOG_W("key report could not be queued\r\n");
    }
}

Svc_StatusTypeDef Inter_KeyAttachReport(void)
{
    return Svc_Key_SetEventSink(Inter_KeyOnEvent);
}

#endif /* APP_ENABLE_KEY */

/* ========================================================================== */
/*  命令地图(谁能认领什么命令, 只写在这一处)                                    */
/* ========================================================================== */

/**
 * 精确名(整行相等): STATUS —— net 先查这一类。
 * 兜底项(Name == NULL): 按顺序依次问, 各自的 handler 用返回 0 把自己摘干净。
 *   按键 0xAA..0xAC / 制动 0xA0..0xA5 / 发射 0xD0..0xD6 /
 *   采样 0xE0..0xE2 / 呼吸灯 0xF0..0xFF
 * 各段字节互不重叠, 顺序无所谓; 处理代码仍留在各模块自己的文件里。
 */
const Svc_Net_CmdTypeDef Inter_App_Commands[] = {
    /* ---- 精确名 ---- */
    { "STATUS", Inter_StatusCommand, NULL, "one line status" },

    /* ---- 兜底 ---- */
    /* 电流指令帧 {0xBA, lo, hi, 0xAB}: 只认完整的 4 字节帧, 其它一律放行 */
    { NULL, Current_App_Command, NULL,
      "0xBA..0xAB current command frame (target current mA, shared by 5 bridges)" },
#if (APP_ENABLE_KEY != 0U)
    { NULL, Inter_KeyCommand, NULL, "0xAA..0xAC ps keys (status / soft reset / net restart)" },
#endif
    { NULL, BrakeCtl_App_Command, NULL,
      "0xA0..0xA5 brake (enable/start/preset/stop) + frames 0xDA..0xDB duty, 0xDC..0xDD dead time" },
#if (APP_ENABLE_MOTOR != 0U)
    { NULL, Mover_App_Command, NULL, "0xD0..0xD6 mover (launch / SS1..SS6)" },
#endif
#if (APP_ENABLE_ADS != 0U)
    { NULL, Samp_App_Command,  NULL, "0xE0..0xE2 samp (start/stop/status)" },
#endif
    { NULL, Monitor_App_Command, NULL,
      "0xF0..0xFF leds (all off / slow / normal / fast / status / all on / on0..3 / "
      "off0..3 / 0xFE+Hz/10 / 0xFF+mask)" },
};

const uint16_t Inter_App_CommandCount = (uint16_t)(sizeof(Inter_App_Commands) /
                                                   sizeof(Inter_App_Commands[0]));
