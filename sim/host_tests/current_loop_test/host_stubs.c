/**
 *******************************************************************************
 * @file    host_stubs.c
 * @brief   主机测试的桩实现: ADS 反馈 / PWM 输出 / Osal_DelayMs / 日志
 *******************************************************************************
 */

#include "host_stubs.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "service/brake_module/service_pwm.h"
#include "service/Samp_module/service_ads.h"
#include "service/Algorithm_module/service_current_loop.h"

/* ========================================================================== */
/*  单步执行                                                                   */
/* ========================================================================== */

jmp_buf         gHostJmp;
volatile int    gHostStepLimit = 0;
volatile int    gHostSteps     = 0;

void Host_Step(int Cycles)
{
    gHostStepLimit = Cycles;
    gHostSteps     = 0;
}

void Host_RunLoop(int Cycles)
{
    Host_Step(Cycles);

    if (setjmp(gHostJmp) == 0) {
        Svc_CurLoop_Task(NULL);
    }
}

/* ========================================================================== */
/*  伪造的电流反馈                                                             */
/* ========================================================================== */

static int32_t gFakeMa[5];

void Host_SetCurrentMa(int Coil, int32_t MilliAmps)
{
    if ((Coil >= 0) && (Coil < 5)) {
        gFakeMa[Coil] = MilliAmps;
    }
}

void Host_SetAllCurrentMa(int32_t MilliAmps)
{
    int i;

    for (i = 0; i < 5; i++) {
        gFakeMa[i] = MilliAmps;
    }
}

Svc_StatusTypeDef Svc_Ads_GetCoilCurrentMa(int32_t *Out, uint32_t Count)
{
    uint32_t i;

    if (Out == NULL) {
        return SVC_ERR_PARAM;
    }

    for (i = 0U; (i < Count) && (i < SVC_ADS_COILS); i++) {
        Out[i] = gFakeMa[i];
    }

    return SVC_OK;
}

/* ========================================================================== */
/*  记录下来的 PWM 输出                                                         */
/* ========================================================================== */

static uint32_t gDutyPermille[5];
static int      gBridgeEnable;
static int      gGlobalEnable;
static uint32_t gDutyWrites;
static uint32_t gEnableWrites;
static uint32_t gDeadNs;
static uint32_t gDeadWrites;

Svc_StatusTypeDef Svc_Pwm_SetBridgeBrakePermille(int Bridge, uint32_t Permille)
{
    if ((Bridge < 0) || (Bridge >= 5)) {
        return SVC_ERR_PARAM;
    }
    if (Permille > 1000U) {
        return SVC_ERR_PARAM;
    }

    gDutyPermille[Bridge] = Permille;
    gDutyWrites++;

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Pwm_SetAllDeadtimeNs(uint32_t Ns)
{
    gDeadNs = Ns;
    gDeadWrites++;

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Pwm_SetAllBridgeEnable(int Enable)
{
    gBridgeEnable = (Enable != 0) ? 1 : 0;
    gEnableWrites++;

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Pwm_SetGlobalEnable(int Enable)
{
    gGlobalEnable = (Enable != 0) ? 1 : 0;
    gEnableWrites++;

    return SVC_OK;
}

uint32_t Host_GetDutyPermille(int Bridge)
{
    if ((Bridge < 0) || (Bridge >= 5)) {
        return 0U;
    }

    return gDutyPermille[Bridge];
}

int      Host_GetBridgeEnable(void) { return gBridgeEnable; }
int      Host_GetGlobalEnable(void) { return gGlobalEnable; }
uint32_t Host_GetDutyWrites(void)   { return gDutyWrites;   }
uint32_t Host_GetEnableWrites(void) { return gEnableWrites; }
uint32_t Host_GetDeadNs(void)       { return gDeadNs;       }
uint32_t Host_GetDeadWrites(void)   { return gDeadWrites;   }

void Host_ApplyManualDeadNs(uint32_t Ns)
{
    (void)Svc_Pwm_SetAllDeadtimeNs(Ns);
}

void Host_ForcePowerStageOff(void)
{
    gBridgeEnable = 0;
    gGlobalEnable = 0;
}

/* ========================================================================== */
/*  OSAL / 日志桩                                                              */
/* ========================================================================== */

void Osal_DelayMs(uint32_t Ms)
{
    (void)Ms;

    gHostSteps++;
    if (gHostSteps >= gHostStepLimit) {
        longjmp(gHostJmp, 1);           /* 拍数到了: 回到测试代码做断言 */
    }
}

static int gLogVerbose;

void Host_SetLogVerbose(int On)
{
    gLogVerbose = (On != 0) ? 1 : 0;
}

void Host_Log(const char *Fmt, ...)
{
    va_list ap;

    if (gLogVerbose == 0) {
        return;
    }

    fputs("[curloop] ", stdout);

    va_start(ap, Fmt);
    vprintf(Fmt, ap);
    va_end(ap);
}
