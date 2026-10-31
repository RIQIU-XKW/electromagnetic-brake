/**
 *******************************************************************************
 * @file    samp_app.c
 * @brief   采集业务: 单字节命令 0xE0..0xE3 的处理 + ads/emmc 两条任务的创建
 *
 *   0xE0 开始采样 / 0xE1 停止采样 / 0xE2 查状态(一行文本)
 *   0xE3 电流零点标定: 采当前引脚电压当"零电流电压", 之后静止时读数为 0
 *        (要求: 采样中 + 制动关闭)
 *   回话是文本, 上位机接收区直接可读。
 *******************************************************************************
 */

#define LOG_ENABLE 1   /* 命令与状态各打一行 */
#define LOG_TAG "samp"
#include "service/monitor_module/log/log.h"

#include "app/samp_app.h"
#include "app/app_config.h"                        /* APP_ENABLE_ADS / APP_ENABLE_EMMC */
#include "app/brake_ctl_app.h"                     /* 0xE3 要先确认制动是关的 */

#include "service/Samp_module/service_ads.h"       /* 采样开关 + 状态 */
#if (APP_ENABLE_EMMC != 0U)
#include "service/Samp_module/service_emmc.h"      /* 录制情况 */
#endif
#include "service/inter_module/net_text.h"         /* 拼回话文本 */
#include "osal/osal.h"

#include <string.h>

/* ========================================================================== */
/*  任务(ads 先起, emmc 后起; emmc 优先级故意比 ads 低, 写盘不许抢采样节拍)      */
/* ========================================================================== */

Svc_StatusTypeDef Samp_App_Start(void)
{
#if (APP_ENABLE_ADS != 0U)
    if (Svc_Ads_IsReady() != 0U) {
        if (Osal_TaskCreate("ads", Svc_Ads_Task, NULL,
                            SVC_ADS_STACK_WORDS, SVC_ADS_PRIO, NULL) != OSAL_OK) {
            LOG_E("ads task creation failed\r\n");
            return SVC_ERR_HW;
        }
    } else {
        LOG_W("ads component not ready - sampling task skipped\r\n");
    }
#endif

#if (APP_ENABLE_EMMC != 0U)
    if (Svc_Emmc_IsReady() != 0U) {
        if (Osal_TaskCreate("emmc", Svc_Emmc_Task, NULL,
                            SVC_EMMC_STACK_WORDS, SVC_EMMC_PRIO, NULL) != OSAL_OK) {
            LOG_W("emmc task creation failed (recording unavailable)\r\n");
        }
    } else {
        LOG_I("emmc component not started (no card?) - recording unavailable\r\n");
    }
#endif

    return SVC_OK;
}

/* ========================================================================== */
/*  采样开关                                                                   */
/* ========================================================================== */

Svc_StatusTypeDef Samp_App_StartSampling(void)
{
    return Svc_Ads_SetRunning(1);
}

Svc_StatusTypeDef Samp_App_StopSampling(void)
{
    return Svc_Ads_SetRunning(0);
}

int Samp_App_IsSampling(void)
{
    return Svc_Ads_IsRunning();
}

/* ========================================================================== */
/*  命令处理: 0xE0..0xE2(单字节)                                                */
/* ========================================================================== */

/** @brief  0xE2: 一行状态(在不在采 / 在不在发 / 录制情况)。 */
static void Samp_App_ReplyStatus(Svc_Net_ReplyTypeDef *R)
{
    char     text[SVC_NET_MSG_MAX];
    uint16_t pos = 0U;
#if (APP_ENABLE_EMMC != 0U)
    Svc_Emmc_SnapshotTypeDef emmc;

    (void)memset(&emmc, 0, sizeof(emmc));
    (void)Svc_Emmc_GetSnapshot(&emmc);
#endif

    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "samp E2 sampling=");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                             (Svc_Ads_IsRunning() != 0) ? "ON" : "OFF");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " stream=");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                             (Svc_Ads_IsStreaming() != 0) ? "ON" : "OFF");

#if (APP_ENABLE_EMMC != 0U)
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " emmc=");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                             (emmc.Recording != 0U) ? "recording" : "idle");
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " written=");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), emmc.Written);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " dropped=");
    pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), emmc.Dropped);
#else
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " emmc=off (not built in)");
#endif

    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "\r\n");

    (void)Svc_Net_Reply(R, text, pos);
}

/**
 * @brief  处理 0xE0..0xE2(由 inter_app 的命令地图调用)。
 *
 * 按字节扫整个报文(上位机可能把几个字节粘在一个报文里), 处理一个就返回。
 * 没有本模块的字节返回 0 放行 —— 文本命令全是 ASCII, 不会撞上 0xE0..0xE2。
 */
int Samp_App_Command(void *Arg, const uint8_t *Data, uint16_t Len,
                            Svc_Net_ReplyTypeDef *R)
{
    uint16_t i;

    (void)Arg;

    if ((Data == NULL) || (Len == 0U)) {
        return 0;
    }

    for (i = 0U; i < Len; i++) {
        const uint8_t code = Data[i];

        if ((code < SAMP_CMD_START) || (code > SAMP_CMD_ZERO)) {
            continue;                       /* 不是本模块的字节 */
        }

        switch (code) {
        case SAMP_CMD_START:
            (void)Svc_Ads_SetRunning(1);
            (void)Svc_Net_Reply(R, "samp E0 sampling ON (a DATA line every 10 ms)\r\n", 0U);
            break;

        case SAMP_CMD_STOP:
            (void)Svc_Ads_SetRunning(0);
            (void)Svc_Net_Reply(R, "samp E1 sampling OFF (bus quiet)\r\n", 0U);
            break;

        case SAMP_CMD_ZERO:
            /*
             * 零点标定: 把**当前**这一路的引脚电压当作"零电流电压"。
             * 所以必须在"制动关闭、线圈无电流"的时候做, 否则标出来的是带负载的电压。
             */
            {
                BrkCtl_SnapshotTypeDef brk;

                (void)BrakeCtl_App_GetSnapshot(&brk);

                if (Svc_Ads_IsRunning() == 0) {
                    (void)Svc_Net_Reply(R,
                        "samp E3 REFUSED: start sampling first (0xE0)\r\n", 0U);
                } else if ((brk.Init != 0U) && (brk.On != 0U)) {
                    (void)Svc_Net_Reply(R,
                        "samp E3 REFUSED: brake is ON - stop it (0xA5) before zeroing\r\n", 0U);
                } else if (Svc_Ads_RequestZeroCalib(0U) != SVC_OK) {
                    (void)Svc_Net_Reply(R, "samp E3 REFUSED: cannot start calibration\r\n", 0U);
                } else {
                    (void)Svc_Net_Reply(R,
                        "samp E3 zero calibration started (200 rounds = 2 s, brake must stay OFF)\r\n",
                        0U);
                }
            }
            break;

        case SAMP_CMD_STATUS:
        default:
            Samp_App_ReplyStatus(R);
            break;
        }

        LOG_I("0x%02X handled (sampling %s)\r\n",
              (unsigned)code, (Svc_Ads_IsRunning() != 0) ? "ON" : "OFF");

        return 1;               /* 认领了(一个报文只处理一次) */
    }

    return 0;                   /* 没有本模块的字节 */
}
