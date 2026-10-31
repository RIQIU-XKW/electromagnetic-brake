/**
 *******************************************************************************
 * @file    service_pwm.c
 * @brief   pwm 业务组件 —— PL 功率级命令处理(原 app_net.c 里的 App_Net_HandlePwm)
 *
 * 回话文本与重构前逐字一致, 便于对照 README 和现场记录排查。
 *******************************************************************************
 */

#define LOG_ENABLE 1   /* 排障期打开: 频率/死区/ID 校验这些初始化信息 */
#define LOG_TAG "pwm"
#include "service/monitor_module/log/log.h"

#include "service/brake_module/service_pwm.h"
#include "service/inter_module/net_text.h"

/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

static Pwm_HandleTypeDef gPwm;
static uint8_t           gReady;

/* ========================================================================== */
/*  对外接口                                                                */
/* ========================================================================== */

Svc_StatusTypeDef Svc_Pwm_Init(void)
{
    int i;

    if (Pwm_Init(&gPwm, PWM_BASEADDR, PWM_ACLK_HZ) != PWM_AXI_OK) {
        LOG_E("PWM IP init failed at 0x%08X\r\n", (unsigned)PWM_BASEADDR);
        return SVC_ERR_HW;
    }

    if (Pwm_SetFrequencyHz(&gPwm, SVC_PWM_DEFAULT_FREQ_HZ) != PWM_AXI_OK) {
        LOG_E("cannot set the default %u Hz - period out of range?\r\n",
              (unsigned)SVC_PWM_DEFAULT_FREQ_HZ);
        return SVC_ERR_HW;
    }
    for (i = 0; i < (int)PWM_AXI_BRIDGE_COUNT; i++) {
        if ((Pwm_SetDeadtimeNs(&gPwm, i, PWM_AXI_LEG_A, SVC_PWM_DEFAULT_DEADTIME_NS) != PWM_AXI_OK) ||
            (Pwm_SetDeadtimeNs(&gPwm, i, PWM_AXI_LEG_B, SVC_PWM_DEFAULT_DEADTIME_NS) != PWM_AXI_OK)) {
            LOG_E("cannot set the default deadtime on bridge %d\r\n", i);
            return SVC_ERR_HW;
        }
    }

    gReady = 1U;

    LOG_I("pwm ready: %u Hz (period %u cycles @ %u Hz aclk), deadtime %u ns, %u bridges\r\n",
          (unsigned)Pwm_GetFrequencyHz(&gPwm),
          (unsigned)Pwm_GetPeriodCycles(&gPwm),
          (unsigned)PWM_ACLK_HZ,
          (unsigned)SVC_PWM_DEFAULT_DEADTIME_NS,
          (unsigned)PWM_AXI_BRIDGE_COUNT);

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Pwm_CheckId(void)
{
    uint32_t id;

    if (gReady == 0U) {
        return SVC_ERR_STATE;
    }

    id = Pwm_ReadId(&gPwm);
    if (id != PWM_AXI_ID_VALUE) {
        LOG_E("PWM ID mismatch: 0x%08X (expect 0x%08X) - check the bitstream\r\n",
              (unsigned)id, (unsigned)PWM_AXI_ID_VALUE);
        return SVC_ERR_HW;
    }

    LOG_I("PWM IP ready: id 0x%08X, %u Hz aclk, %u bridges\r\n",
          (unsigned)id, (unsigned)PWM_ACLK_HZ, (unsigned)PWM_AXI_BRIDGE_COUNT);

    return SVC_OK;
}

Pwm_HandleTypeDef *Svc_Pwm_GetHandle(void)
{
    return (gReady != 0U) ? &gPwm : NULL;
}

Svc_StatusTypeDef Svc_Pwm_GetSnapshot(Svc_Pwm_SnapshotTypeDef *Out)
{
    if (Out == NULL) {
        return SVC_ERR_PARAM;
    }

    if (gReady == 0U) {
        return SVC_ERR_STATE;
    }

    Out->EnableMask   = Pwm_GetEnableMask(&gPwm);
    Out->FrequencyHz  = Pwm_GetFrequencyHz(&gPwm);
    Out->PeriodCycles = Pwm_GetPeriodCycles(&gPwm);
    Out->OcNow        = Pwm_OcNow(&gPwm);
    Out->OcLatched    = Pwm_OcLatched(&gPwm);

    return SVC_OK;
}

void Svc_Pwm_AlarmPoll(int AutoAck)
{
    if (gReady != 0U) {
        Pwm_Alarm_Poll(&gPwm, AutoAck);
    }
}

/* ========================================================================== */
/*  制动控制(给 app/brake_app 用的高层接口)                                     */
/* ========================================================================== */

/**
 * @brief  桥号合法性检查.
 *
 * 为什么每个入口都要查: 这些函数是给 app 调的高层接口, 一个写错的桥号(例如 5)
 * 在底层会去写越界的寄存器偏移 —— 宁可返回 SVC_ERR_PARAM 让调用方看见。
 */
static int Svc_Pwm_BridgeOk(int Bridge)
{
    return ((Bridge >= 0) && ((uint32_t)Bridge < (uint32_t)PWM_AXI_BRIDGE_COUNT)) ? 1 : 0;
}

Svc_StatusTypeDef Svc_Pwm_SetGlobalEnable(int Enable)
{
    if (gReady == 0U) {
        return SVC_ERR_STATE;
    }

    Pwm_GlobalEnable(&gPwm, (Enable != 0) ? 1 : 0);

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Pwm_SetBridgeDuty(int Bridge, uint32_t Percent)
{
    if (gReady == 0U) {
        return SVC_ERR_STATE;
    }

    if ((Svc_Pwm_BridgeOk(Bridge) == 0) || (Percent > 100U)) {
        return SVC_ERR_PARAM;
    }

    /*
     * **对角驱动 + 给定占空比** —— 两步都要, 而且顺序不能反:
     *   1. Pwm_SetLegBMode(NORMAL): 腿 B 回到"对角驱动"模式(= 0, 芯片默认值);
     *      ⚠ 不要用 bsp 的 Pwm_CoilDrive(): 那个会把腿 B 设成 STATIC_LOW(1),
     *        那是"纯直流/单腿斩波"的模式, **不是对角驱动**。
     *   2. Pwm_SetDutyPermille(): 设比较值(这一步只在有 PERIOD 时才有效,
     *      周期由 Svc_Pwm_Init() 里的 4 kHz 默认值保证)。
     *
     * ⚠ 这一步**不打开输出**: 桥的两条半桥使能位仍然由 Svc_Pwm_SetBridgeEnable() 管。
     */
    if (Pwm_SetLegBMode(&gPwm, Bridge, PWM_AXI_MODE_NORMAL) != PWM_AXI_OK) {
        return SVC_ERR_PARAM;
    }

    if (Pwm_SetDutyPermille(&gPwm, Bridge, Percent * 10U) != PWM_AXI_OK) {
        return SVC_ERR_PARAM;
    }

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Pwm_SetBridgeEnable(int Bridge, int Enable)
{
    if (gReady == 0U) {
        return SVC_ERR_STATE;
    }

    if (Svc_Pwm_BridgeOk(Bridge) == 0) {
        return SVC_ERR_PARAM;
    }

    Pwm_SetBridgeEnable(&gPwm, Bridge, (Enable != 0) ? 1 : 0);

    return SVC_OK;
}

void Svc_Pwm_DisableAllBridges(void)
{
    int b;

    if (gReady == 0U) {
        return;
    }

    for (b = 0; b < (int)PWM_AXI_BRIDGE_COUNT; b++) {
        Pwm_SetBridgeEnable(&gPwm, b, 0);
    }
}

uint32_t Svc_Pwm_GetBridgeDuty(int Bridge)
{
    if ((gReady == 0U) || (Svc_Pwm_BridgeOk(Bridge) == 0)) {
        return 0U;
    }

    /* bsp 给的是千分比, 这里换算回百分比(整数, 和 SetBridgeDuty 的口径一致) */
    return Pwm_GetDutyPermille(&gPwm, Bridge) / 10U;
}

int Svc_Pwm_IsBridgeEnabled(int Bridge)
{
    uint32_t mask;

    if ((gReady == 0U) || (Svc_Pwm_BridgeOk(Bridge) == 0)) {
        return 0;
    }

    /* 一个桥有两条腿(A/B), 两条都关才算"关" */
    mask = PWM_AXI_EN_BIT(PWM_AXI_HB_INDEX(Bridge, PWM_AXI_LEG_A)) |
           PWM_AXI_EN_BIT(PWM_AXI_HB_INDEX(Bridge, PWM_AXI_LEG_B));

    return ((Pwm_GetEnableMask(&gPwm) & mask) != 0U) ? 1 : 0;
}

void Svc_Pwm_PrintStatus(void)
{
    if (gReady == 0U) {
        LOG_W("pwm not initialised - no status to print\r\n");
        return;
    }

    Pwm_PrintStatus(&gPwm);
}

/* ========================================================================== */
/*  制动用法: 腿 A 静态高 + 腿 B 斩波, 五路同步                                 */
/* ========================================================================== */

Svc_StatusTypeDef Svc_Pwm_SetBridgeBrakeDuty(int Bridge, uint32_t Percent)
{
    if (gReady == 0U) {
        return SVC_ERR_STATE;
    }

    if ((Svc_Pwm_BridgeOk(Bridge) == 0) || (Percent > SVC_PWM_DUTY_MAX_PERCENT)) {
        return SVC_ERR_PARAM;
    }

    /*
     * 顺序不能反: 先定模式(腿 A 静态高 / 腿 B 斩波), 再写占空比。
     * 模式只改 PWM_EN[19:10], 不碰半桥使能; 占空比只写 DUTYx —— 都不会打开输出。
     */
    if (Pwm_SetLegBMode(&gPwm, Bridge, PWM_AXI_MODE_LEG_A_STATIC_HIGH) != PWM_AXI_OK) {
        return SVC_ERR_PARAM;
    }

    if (Pwm_SetDutyPercent(&gPwm, Bridge, Percent) != PWM_AXI_OK) {
        return SVC_ERR_PARAM;
    }

    return SVC_OK;
}

/**
 * @brief  同 Svc_Pwm_SetBridgeBrakeDuty(), 但占空比用千分比(0..1000) —— 闭环控制要更细的分辨率。
 */
Svc_StatusTypeDef Svc_Pwm_SetBridgeBrakePermille(int Bridge, uint32_t Permille)
{
    if (gReady == 0U) {
        return SVC_ERR_STATE;
    }

    if ((Svc_Pwm_BridgeOk(Bridge) == 0) || (Permille > 1000U)) {
        return SVC_ERR_PARAM;
    }

    if (Pwm_SetLegBMode(&gPwm, Bridge, PWM_AXI_MODE_LEG_A_STATIC_HIGH) != PWM_AXI_OK) {
        return SVC_ERR_PARAM;
    }

    if (Pwm_SetDutyPermille(&gPwm, Bridge, Permille) != PWM_AXI_OK) {
        return SVC_ERR_PARAM;
    }

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Pwm_SetAllBrakeDuty(uint32_t Percent)
{
    int b;

    if (gReady == 0U) {
        return SVC_ERR_STATE;
    }

    if (Percent > SVC_PWM_DUTY_MAX_PERCENT) {
        return SVC_ERR_PARAM;
    }

    /* 五路一个不落地写成同一个值: 任何一路失败就整体报错, 调用方据此别当成功 */
    for (b = 0; b < (int)PWM_AXI_BRIDGE_COUNT; b++) {
        if (Svc_Pwm_SetBridgeBrakeDuty(b, Percent) != SVC_OK) {
            return SVC_ERR_PARAM;
        }
    }

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Pwm_SetAllDeadtimeNs(uint32_t Ns)
{
    uint32_t cycles;
    int      b;

    if (gReady == 0U) {
        return SVC_ERR_STATE;
    }

    if (Ns > SVC_PWM_DEAD_MAX_NS) {
        return SVC_ERR_PARAM;
    }

    /* ns -> aclk 周期(四舍五入), 再夹到 16 位 */
    cycles = (Ns + (SVC_PWM_NS_PER_ACLK / 2U)) / SVC_PWM_NS_PER_ACLK;
    if (cycles > (uint32_t)PWM_AXI_DEAD_MAX) {
        cycles = (uint32_t)PWM_AXI_DEAD_MAX;
    }

    for (b = 0; b < (int)PWM_AXI_BRIDGE_COUNT; b++) {
        if (Pwm_SetBridgeDeadtime(&gPwm, b, cycles) != PWM_AXI_OK) {
            return SVC_ERR_PARAM;
        }
    }

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Pwm_SetAllBridgeEnable(int Enable)
{
    int b;

    if (gReady == 0U) {
        return SVC_ERR_STATE;
    }

    for (b = 0; b < (int)PWM_AXI_BRIDGE_COUNT; b++) {
        if (Svc_Pwm_SetBridgeEnable(b, Enable) != SVC_OK) {
            return SVC_ERR_PARAM;
        }
    }

    return SVC_OK;
}

uint32_t Svc_Pwm_GetDeadtimeCycles(int Bridge, int Leg)
{
    if ((gReady == 0U) || (Svc_Pwm_BridgeOk(Bridge) == 0)) {
        return 0U;
    }

    return Pwm_GetDeadtimeCycles(&gPwm, Bridge, Leg);
}

int Svc_Pwm_GetLegBMode(int Bridge)
{
    if ((gReady == 0U) || (Svc_Pwm_BridgeOk(Bridge) == 0)) {
        return -1;
    }

    return (int)Pwm_GetLegBMode(&gPwm, Bridge);
}
