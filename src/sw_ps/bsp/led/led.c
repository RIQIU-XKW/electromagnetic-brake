/**
 *******************************************************************************
 * @file    led.c
 * @brief   LED module — breath_led IP core driver implementation
 *
 * Plain memory mapped register access: the IP exposes no driver instance, so
 * the handle only carries the base address and a shadow of what was written.
 * Keeping the shadow lets the module answer status queries without reading
 * back (the IP does echo its registers, but the shadow is cheaper and works
 * even if the core is reset by a bitstream reload).
 *******************************************************************************
 */

#include "led.h"

#include "xil_io.h"
/* log tag: LOG_I/W/E from this file print with the [led] prefix */
#define LOG_ENABLE 0   /* 本模块日志已关闭; 需要时改成 1 */
#define LOG_TAG "led"
#include "service/monitor_module/log/log.h"

#include "xil_printf.h"

/* ========================================================================== */
/*  内部函数(static)                                                              */
/* ========================================================================== */

/**
 * @brief  Write the control register (four enables) and refresh the shadow.
 * @param  h    : Initialised handle.
 * @param  mask : bit[3:0] = LED0..LED3 使能; 高位被丢掉。
 *
 * 一次写完整个掩码 = 唯一改 CTRL 的地方, 所以影子 Mask / Enabled 永远不会和
 * 寄存器脱节(以前 Enabled 要在每个调用点各自设置, 漏一次就和硬件对不上)。
 */
static void Led_WriteCtrl(Led_HandleTypeDef *h, uint8_t mask)
{
    mask &= LED_MASK_ALL;

    Xil_Out32(h->BaseAddr + LED_REG_CTRL, (uint32_t)mask);

    h->Mask    = mask;
    h->Enabled = (mask != 0U) ? 1U : 0U;
    h->Writes++;
}

/**
 * @brief  Clamp a frequency to what the IP supports —— **唯一的夹取点**。
 * @param  freq : Requested frequency.
 * @return freq clamped to LED_FREQ_MIN .. LED_FREQ_MAX.
 *
 * 夹取只在这里做, 而且调用者**先夹再存影子**(h->Freq = Led_ClampFreq(freq))。
 * 以前是 "影子存原值、只有写寄存器时夹", 结果 Led_SetFreq(5000) 之后
 * Led_GetState() 读出 5000, 而硬件实际按 1000 跑 —— 状态行/STATUS 会报一个板子
 * 上根本不存在的频率, 上位机按它做换算就全错了。影子要么等于硬件, 要么就别叫
 * "影子"。
 */
static uint16_t Led_ClampFreq(uint16_t freq)
{
    if (freq < LED_FREQ_MIN) {
        return LED_FREQ_MIN;
    }

    if (freq > LED_FREQ_MAX) {
        return LED_FREQ_MAX;
    }

    return freq;
}

/**
 * @brief  Write the breathing period register (clamped to the IP limits).
 * @param  h    : Initialised handle.
 * @param  freq : Frequency to program.
 */
static void Led_WritePeriod(Led_HandleTypeDef *h, uint16_t freq)
{
    Xil_Out32(h->BaseAddr + LED_REG_PERIOD, (uint32_t)Led_ClampFreq(freq));
    h->Writes++;
}

/* ========================================================================== */
/*  对外接口                                                                      */
/* ========================================================================== */

uint16_t Led_ModeToFreq(Led_ModeTypeDef mode)
{
    switch (mode) {
        case LED_MODE_BREATH_SLOW:   return 20U;    /* ~5 s cycle              */
        case LED_MODE_BREATH_NORMAL: return 100U;   /* ~1 s cycle              */
        case LED_MODE_BREATH_FAST:   return 1000U;  /* ~100 ms cycle           */
        case LED_MODE_OFF:           /* fall through: frequency is irrelevant  */
        default:                     return LED_FREQ_MIN;
    }
}

Led_ModeTypeDef Led_FreqToMode(uint16_t freq)
{
    if (freq <= 40U) {
        return LED_MODE_BREATH_SLOW;
    }

    if (freq <= 400U) {
        return LED_MODE_BREATH_NORMAL;
    }

    return LED_MODE_BREATH_FAST;
}

const char *Led_ModeName(Led_ModeTypeDef mode)
{
    switch (mode) {
        case LED_MODE_OFF:           return "off";
        case LED_MODE_BREATH_SLOW:   return "slow";
        case LED_MODE_BREATH_NORMAL: return "normal";
        case LED_MODE_BREATH_FAST:   return "fast";
        default:                     return "?";
    }
}

Led_StatusTypeDef Led_Init(Led_HandleTypeDef *h, uint32_t base)
{
    if (h == NULL) {
        return LED_ERR_PARAM;
    }

    h->BaseAddr = (base != 0U) ? base : LED_BASEADDR;
    h->Enabled  = 0U;
    h->Mask     = 0U;
    h->Mode     = LED_MODE_OFF;
    h->Freq     = Led_ModeToFreq(LED_MODE_BREATH_NORMAL);
    h->Writes   = 0U;

    /* Program a sane period first, then make sure all four LEDs are dark. */
    Led_WritePeriod(h, h->Freq);
    Led_WriteCtrl(h, 0U);

    LOG_I("breath_led IP at 0x%08X, %u LEDs (modes: off/slow/normal/fast)\r\n",
               (unsigned)h->BaseAddr, (unsigned)LED_COUNT);

    return LED_OK;
}

Led_StatusTypeDef Led_SetMask(Led_HandleTypeDef *h, uint8_t mask)
{
    if (h == NULL) {
        return LED_ERR_PARAM;
    }

    Led_WriteCtrl(h, mask);

    return LED_OK;
}

Led_StatusTypeDef Led_SetEnable(Led_HandleTypeDef *h, uint8_t index, uint8_t on)
{
    uint8_t mask;

    if ((h == NULL) || (index >= LED_COUNT) || (on > 1U)) {
        return LED_ERR_PARAM;
    }

    mask = h->Mask;

    if (on != 0U) {
        mask = (uint8_t)(mask | LED_MASK_OF(index));
    } else {
        mask = (uint8_t)(mask & (uint8_t)~LED_MASK_OF(index));
    }

    Led_WriteCtrl(h, mask);

    return LED_OK;
}

uint8_t Led_GetMask(const Led_HandleTypeDef *h)
{
    return (h != NULL) ? h->Mask : 0U;
}

Led_StatusTypeDef Led_SetMode(Led_HandleTypeDef *h, Led_ModeTypeDef mode)
{
    if (h == NULL) {
        return LED_ERR_PARAM;
    }

    /* Led_ModeTypeDef is an unsigned enum, so only the upper bound needs a
     * check: a peer that sends 0x80..0xFF lands here and is rejected. */
    if (mode >= LED_MODE_COUNT) {
        return LED_ERR_PARAM;
    }

    h->Mode = mode;

    if (mode == LED_MODE_OFF) {
        /* "关灯" 就是四路全灭 —— 不是只关指示灯那一颗。 */
        Led_WriteCtrl(h, 0U);
        return LED_OK;
    }

    h->Freq = Led_ClampFreq(Led_ModeToFreq(mode));
    Led_WritePeriod(h, h->Freq);

    /* 掩码为 0 时点亮指示灯(LED0); 已经有灯亮着就原样保留, 免得顺手关掉用户
     * 用 Led_SetEnable() 点亮的其它三路。 */
    Led_WriteCtrl(h, (h->Mask != 0U) ? h->Mask : LED_MASK_INDICATOR);

    return LED_OK;
}

Led_StatusTypeDef Led_SetFreq(Led_HandleTypeDef *h, uint16_t freq)
{
    if (h == NULL) {
        return LED_ERR_PARAM;
    }

    if (freq == 0U) {
        return LED_ERR_PARAM;
    }

    /* 先夹再存: 影子要反映硬件真正跑的值, 否则 Led_GetState() 会报出一个 IP 根本
     * 没接受的频率(见 Led_ClampFreq 的说明)。 */
    h->Freq = Led_ClampFreq(freq);
    h->Mode = Led_FreqToMode(h->Freq);

    Led_WritePeriod(h, h->Freq);

    /*
     * !! 必须把 CTRL 也使能上。
     *
     * IP 只有两个寄存器: CTRL.bit[3:0] = 四路通断, PERIOD = 共享呼吸频率。只写
     * PERIOD 而 CTRL 还是 0 的话, 灯**根本不会亮** —— 而 Led_Init() 结束时 CTRL
     * 就是 0。
     *
     * 这个坑 2026 年踩过一次: 上位机连上来要点"网口在线"呼吸灯, 走的是
     * "Freq != 0 -> Led_SetFreq()" 这条路, 结果灯一直不亮。以前没暴露是因为
     * LED_STA_0..3 都走 Led_SetMode()(那条路会写 CTRL), Freq != 0 这条路从来
     * 没人用过, 属于死代码里的 bug。
     *
     * 语义上也应该使能: 头文件里写了本模块"要么灭要么呼吸", 设了频率却还是全灭
     * 的状态是个说不通的状态, 而且和 Led_SetMode(mode != OFF) 的行为不一致。
     *
     * 四路扩宽后这里改成"掩码为 0 才点亮指示灯": 有灯亮着就按当前掩码重写 CTRL,
     * 不去覆盖用户的选择(LEDON/LEDMASK 设过的位要保持)。改频率不应该顺带点灯或
     * 关灯, 只应该保证"不是全灭"。
     */
    Led_WriteCtrl(h, (h->Mask != 0U) ? h->Mask : LED_MASK_INDICATOR);

    return LED_OK;
}

Led_StatusTypeDef Led_GetState(const Led_HandleTypeDef *h, Led_ModeTypeDef *mode,
                               uint16_t *freq, uint8_t *enabled)
{
    if (h == NULL) {
        return LED_ERR_PARAM;
    }

    if (mode != NULL) {
        *mode = h->Mode;
    }
    if (freq != NULL) {
        *freq = h->Freq;
    }
    if (enabled != NULL) {
        *enabled = h->Enabled;
    }

    return LED_OK;
}
