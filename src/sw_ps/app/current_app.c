/**
 *******************************************************************************
 * @file    current_app.c
 * @brief   电流指令帧 {0xBA, mA_lo, mA_hi, 0xAB} 的解析 + 电流闭环任务的创建
 *
 *   net 任务(收到帧) -> Svc_CurLoop_SetTargetA() -> curloop 任务(10 ms) ->
 *   Svc_Pwm_SetBridgeBrakePermille() -> PL
 *******************************************************************************
 */

#define LOG_ENABLE 1   /* 收到指令打一行 */
#define LOG_TAG "curap"
#include "service/monitor_module/log/log.h"

#include "app/current_app.h"

#include "service/Algorithm_module/service_current_loop.h"
#include "service/inter_module/net_text.h"         /* 拼回话文本 */
#include "osal/osal.h"

/* ========================================================================== */
/*  对外接口                                                                   */
/* ========================================================================== */

Svc_StatusTypeDef Current_App_Init(void)
{
    return Svc_CurLoop_Init();
}

Svc_StatusTypeDef Current_App_Start(void)
{
    if (Osal_TaskCreate("curloop", Svc_CurLoop_Task, NULL,
                        SVC_CURLOOP_STACK_WORDS, SVC_CURLOOP_PRIO, NULL) != OSAL_OK) {
        LOG_E("current loop task creation failed\r\n");
        return SVC_ERR_HW;
    }

    return SVC_OK;
}

int Current_App_Command(void *Arg, const uint8_t *Data, uint16_t Len,
                        Svc_Net_ReplyTypeDef *R)
{
    uint16_t i;

    (void)Arg;

    if ((Data == NULL) || (Len < 4U)) {
        return 0;                       /* 帧不完整 -> 交给后面的兜底项 */
    }

    for (i = 0U; (uint16_t)(i + 4U) <= Len; i++) {
        char     text[96];
        uint16_t pos   = 0U;
        uint16_t ma;
        float    amps;

        if (Data[i] != CURAPP_FRAME_HEAD) {
            continue;
        }

        if (Data[i + 3U] != CURAPP_FRAME_TAIL) {
            if (R != NULL) {
                (void)Svc_Net_Reply(R,
                    "current ERR: frame must be [0xBA][lo][hi][0xAB]\r\n", 0U);
            }
            return 1;                   /* 认领: 这是想发电流指令但帧尾错了 */
        }

        /* 低字节在前 */
        ma = (uint16_t)((uint16_t)Data[i + 1U] | ((uint16_t)Data[i + 2U] << 8));
        if (ma > CURAPP_CURRENT_MAX_MA) {
            ma = CURAPP_CURRENT_MAX_MA;     /* 上限就是 15 A */
        }

        amps = (float)ma / 1000.0f;
        (void)Svc_CurLoop_SetTargetA(amps);

        LOG_I("current target %.3f A (%u mA) -> 5 loops\r\n", (double)amps, (unsigned)ma);

        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "current target ");
        pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), (uint32_t)ma);
        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " mA (5 bridges)\r\n");
        if (R != NULL) {
            (void)Svc_Net_Reply(R, text, pos);
        }

        return 1;
    }

    return 0;
}
