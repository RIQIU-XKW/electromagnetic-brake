/**
 *******************************************************************************
 * @file    service_oc.c
 * @brief   过流(OC)业务服务实现 —— 采样 bsp/oc, 维护"此刻/累计/事件"三个位图
 *
 * 三个位图的分工(别混):
 *   NowMask    : 这一拍硬件说"有输入在拉低"的桥 —— 用来判断**现在**要不要保护
 *   Latched    : 硬件粘滞位 —— 过的事件跑不掉, 直到 Acknowledge
 *   EventMask  : 本组件自己累计的"出现过"的桥 —— 给上位机 0xC 上报和日志用
 *******************************************************************************
 */

#define LOG_ENABLE 1   /* 排障期打开: 上电一行 + **过流状态每变化一次一行**(见 Svc_Oc_Poll 末尾) */
#define LOG_TAG "oc"
#include "service/monitor_module/log/log.h"

#include "service/brake_module/service_oc.h"
#include "service/brake_module/service_pwm.h"    /* 取 pwm 句柄给 bsp/oc 绑 */
#include "bsp/oc/bsp_oc.h"

static uint8_t  gReady;
static uint8_t  gAlarm;                 /**< 1 = 此刻告警中 */
static uint32_t gNowMask;
static uint32_t gLatchedMask;
static uint32_t gEventMask;             /**< 软件累计(本次上电以来) */
static uint32_t gPolls;
static uint8_t  gPendingEvent;  /**< 这次过流事件上层处理过没有(用来决定什么时候清硬件锁存) */

/* 上一次打过日志的三个量: 用来做"只在变化时打一行"(见 Svc_Oc_Poll 末尾) */
static uint32_t gLogLastNow;
static uint32_t gLogLastLatched;
static uint32_t gLogLastEvent;

Svc_StatusTypeDef Svc_Oc_Init(void)
{
    Pwm_HandleTypeDef *pwm = Svc_Pwm_GetHandle();

    if (pwm == NULL) {
        LOG_E("pwm component is not initialised - call Svc_Pwm_Init() first\r\n");
        return SVC_ERR_HW;
    }

    Bsp_Oc_Init(pwm);

    gReady       = 1U;
    gAlarm       = 0U;
    gNowMask     = 0U;
    gLatchedMask = 0U;
    gEventMask   = 0U;
    gPolls       = 0U;

    /*
     * ⚠ 上电就把过流状态打出来 —— 这是"桥没有输出"时最该先看的一行:
     *   PL 的 PWM IP 里有 `oc_now` 逻辑: 只要有一路 oc_n 为低, 硬件会把 20 路输出
     *   **直接压低**(不受软件控制)。所以如果这里 `now=1`, 桥是不可能输出的,
     *   得先查那 5 路过流输入(没接/常低/接线) —— 而不是怀疑占空比。
     */
    LOG_I("ready: %u channels, status 0x%08X, oc now=%u latched=%u src=0x%02X\r\n",
          (unsigned)SVC_OC_CHANNELS, (unsigned)Bsp_Oc_RawStatus(),
          (unsigned)Bsp_Oc_AnyNow(), (unsigned)Bsp_Oc_AnyLatched(),
          (unsigned)Bsp_Oc_LatchedMask());

    return SVC_OK;
}

void Svc_Oc_Poll(void)
{
    uint32_t now;
    uint32_t latched;
    uint8_t  anyNow;
    uint8_t  anyLatched;

    if (gReady == 0U) {
        return;
    }

    gPolls++;

    /*
     * ⚠ 一拍都不等: 硬件已经给了"此刻有输入拉低"(OC_NOW)和"分桥粘滞位", 软件不再滤波 ——
     *   再加延时只会推迟保护动作。
     */
    anyNow     = (Bsp_Oc_AnyNow() != 0) ? 1U : 0U;
    anyLatched = (Bsp_Oc_AnyLatched() != 0) ? 1U : 0U;
    latched    = Bsp_Oc_LatchedMask();
    now        = Bsp_Oc_NowMask();

    gLatchedMask = latched;

    /* "哪几路": 正在过流就用此刻的源; 只剩锁存时(脉冲已过去)用锁存里的分桥位 */
    gNowMask = (now != 0U) ? now : latched;

    /*
     * ⚠ **告警状态 = "此刻"**, 不是"锁存" —— 现场要求: 引脚一恢复, 上位机图标就要回"正常"
     *   (而不是等人工确认)。所以 `gAlarm` 只跟 OC_NOW 走; 上报由 brake_app 按边沿发
     *   (出现发 src, 消失发 src=0)。
     */
    gAlarm = (anyNow != 0U) ? 1U : 0U;

    /*
     * 软件累计位: 本次上电以来出现过过流的桥(只有 Acknowledge 才清)。
     * ⚠ 它存在的意义就是"锁住曾经发生这件事" —— 过流常常只有几十微秒, 5 ms 的轮询
     *   可能正好错过"此刻"那一刻, 但硬件锁存位 + 这里的累计位不会漏。
     */
    if ((now != 0U) || (latched != 0U)) {
        gEventMask |= (now | latched) & SVC_OC_MASK_ALL;
    }

    /*
     * 硬件锁存位的自动清理(否则它一直是 1, 下一次过流就看不出来了):
     *   - 还拉着低(anyNow)  -> 不清, 保护现场;
     *   - 已经恢复高(anyLatched 但 anyNow == 0):
     *       第一次看到  -> 先留着(gPendingEvent = 1), 让上层这一拍能把"哪一路"报出去;
     *       第二次看到  -> 上层已经处理过, 现在 acknowledge 硬件锁存。
     *   ⚠ 只在这里清, 而且是"引脚已经恢复"之后 —— 断言期间永远不会清。
     */
    if (anyNow != 0U) {
        gPendingEvent = 1U;
    } else if (anyLatched != 0U) {
        if (gPendingEvent != 0U) {
            (void)Bsp_Oc_Acknowledge();
            gPendingEvent = 0U;
        } else {
            gPendingEvent = 1U;
        }
    } else {
        gPendingEvent = 0U;
    }

    /*
     * ---- 过流日志: **只在状态变化的那一拍打一行** --------------------------
     *
     * ⚠ 这是现场排障的**硬证据**, 比任何"我看到波形没了"都可靠:
     *   过流是微秒级事件, 如果每拍(5 ms)都打会把串口刷爆; 而真正要回答的问题只有
     *   "板子到底有没有看见 oc_n 低、是哪几路" —— 那只需要"变的那一刻"。
     *
     * 字段怎么读:
     *   now     : 硬件 OC_SRC(粘滞, 位 i = 第 i+1 路) —— 非 0 说明**硬件确实抓到过过流**
     *   latched : 硬件粘滞标志 OC_LATCHED(0/1)
     *   ev      : 软件累计位(本次上电以来出现过过流的桥, 只有 Acknowledge 才清)
     *   alarm   : 1 = 此刻引脚还在低(组合逻辑 oc_now)
     *   raw     : STATUS 寄存器原值(0x3C), 出问题时能对着 RTL 逐位对
     *
     * 判读: 如果这里**从来没出现过 now/latched 非 0**, 而现场确实过流了, 那就是
     *   oc_n 那 5 路输入根本没拉低(接线/上拉/驱动板比较器没接过来), 与软件无关。
     */
    if ((now != gLogLastNow) || (latched != gLogLastLatched) ||
        ((gEventMask & SVC_OC_MASK_ALL) != gLogLastEvent)) {
        LOG_I("now=0x%02X latched=0x%02X ev=0x%02X alarm=%u raw=0x%08X\r\n",
              (unsigned)now, (unsigned)latched,
              (unsigned)(gEventMask & SVC_OC_MASK_ALL), (unsigned)gAlarm,
              (unsigned)Bsp_Oc_RawStatus());

        gLogLastNow     = now;
        gLogLastLatched = latched;
        gLogLastEvent   = gEventMask & SVC_OC_MASK_ALL;
    }
}

int Svc_Oc_IsAlarm(void)
{
    return (gAlarm != 0U) ? 1 : 0;
}

uint32_t Svc_Oc_GetNowMask(void)
{
    return gNowMask;
}

uint32_t Svc_Oc_GetLatchedMask(void)
{
    return gLatchedMask;
}

uint32_t Svc_Oc_GetEventMask(void)
{
    return gEventMask;
}

int Svc_Oc_AnyFault(void)
{
    return ((gAlarm != 0U) || ((gEventMask & SVC_OC_MASK_ALL) != 0U)) ? 1 : 0;
}

Svc_StatusTypeDef Svc_Oc_Acknowledge(void)
{
    if (gReady == 0U) {
        return SVC_ERR_STATE;
    }

    if (Bsp_Oc_Acknowledge() == 0) {
        return SVC_ERR_HW;
    }

    gAlarm       = 0U;
    gNowMask     = 0U;
    gLatchedMask = 0U;
    gEventMask   = 0U;

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Oc_GetSnapshot(Svc_Oc_SnapshotTypeDef *Out)
{
    if (Out == NULL) {
        return SVC_ERR_PARAM;
    }

    Out->Ready       = gReady;
    Out->Alarm       = gAlarm;
    Out->NowMask     = gNowMask;
    Out->LatchedMask = gLatchedMask;
    Out->EventMask   = gEventMask;
    Out->Polls       = gPolls;
    Out->RawStatus   = Bsp_Oc_RawStatus();

    return SVC_OK;
}
