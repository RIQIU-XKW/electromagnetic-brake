/**
 *******************************************************************************
 * @file    ls.c
 * @brief   光幕输入模块 — PS EMIO GPIO 读取 + 消抖
 *
 * 只做三件事: 打开 PS GPIO, 把 6 根 LS_I 配成输入, 把引脚电平变成 "哪一路
 * 被遮挡" 这一个字节.  所有硬件细节都在这里, 应用层只看 Ls_Scan() 的返回值.
 *******************************************************************************
 */

#define LOG_ENABLE 0   /* 本模块日志已关闭; 需要时改成 1 */
#define LOG_TAG "ls"
#include "service/monitor_module/log/log.h"

#include "bsp/ls/ls.h"

#include <string.h>

/* ========================================================================== */
/*  内部函数(static)                                                             */
/* ========================================================================== */

/** @brief  取反并裁到 6 位, 用于低有效输入. */
static uint8_t Ls_Invert(uint8_t bits)
{
    return (uint8_t)((~bits) & LS_MASK);
}

/* ========================================================================== */
/*  对外接口                                                                   */
/* ========================================================================== */

Ls_StatusTypeDef Ls_Init(Ls_HandleTypeDef *h, uint32_t baseAddr, uint8_t activeLow)
{
    XGpioPs_Config *config;
    uint32_t        pin;
    uint32_t        base = (baseAddr != 0U) ? baseAddr : LS_GPIO_BASEADDR;

    if (h == NULL) {
        return LS_ERR_PARAM;
    }

    (void)memset(h, 0, sizeof(*h));

    config = XGpioPs_LookupConfig(base);
    if (config == NULL) {
        LOG_E("PS GPIO 0x%08X not found\r\n", (unsigned)base);
        return LS_ERR_HW;
    }

    if (XGpioPs_CfgInitialize(&h->Gpio, config, config->BaseAddr) != XST_SUCCESS) {
        LOG_E("PS GPIO initialisation failed\r\n");
        return LS_ERR_HW;
    }

    /* ---- 6 根线全部配成输入 --------------------------------------------- */
    for (pin = LS_PIN_FIRST; pin < (LS_PIN_FIRST + LS_CHANNELS); pin++) {
        XGpioPs_SetDirectionPin(&h->Gpio, pin, 0U);
    }

    h->ActiveLow = (activeLow != 0U) ? 1U : 0U;
    h->Debounce  = (LS_DEBOUNCE_SCANS != 0U) ? LS_DEBOUNCE_SCANS : 1U;
    h->Ready     = 1U;

    /* 先读一次作为初始状态, 免得把上电瞬间算成一次 "变化". */
    h->Raw   = Ls_ReadRaw(h);
    h->State = (h->ActiveLow != 0U) ? Ls_Invert(h->Raw) : h->Raw;

    LOG_I("LS_I[%u..%u] = PS GPIO %u..%u (EMIO bank2), raw 0x%02X, active-%s, "
          "initial 0x%02X\r\n",
          (unsigned)0U, (unsigned)(LS_CHANNELS - 1U),
          (unsigned)LS_PIN_FIRST, (unsigned)(LS_PIN_FIRST + LS_CHANNELS - 1U),
          (unsigned)h->Raw,
          (h->ActiveLow != 0U) ? "low" : "high",
          (unsigned)h->State);

    if ((h->Raw & LS_MASK) == LS_MASK) {
        LOG_I("all inputs high -> no beam blocked (pull-ups see open inputs)\r\n");
    } else if ((h->Raw & LS_MASK) == 0U) {
        LOG_W("all inputs low -> check the light curtain wiring / EMIO width\r\n");
    }

    return LS_OK;
}

uint8_t Ls_ReadRaw(Ls_HandleTypeDef *h)
{
    uint8_t  bits = 0U;
    uint32_t i;

    if ((h == NULL) || (h->Ready == 0U)) {
        return 0U;
    }

    for (i = 0U; i < LS_CHANNELS; i++) {
        if (XGpioPs_ReadPin(&h->Gpio, LS_PIN_FIRST + i) != 0U) {
            bits |= (uint8_t)(1U << i);
        }
    }

    h->Raw = bits;
    h->Scans++;

    return bits;
}

uint8_t Ls_Scan(Ls_HandleTypeDef *h, uint8_t *changedMask)
{
    uint8_t blocked;
    uint8_t changed = 0U;
    uint8_t i;

    if (changedMask != NULL) {
        *changedMask = 0U;
    }

    if ((h == NULL) || (h->Ready == 0U)) {
        return (h != NULL) ? h->State : 0U;
    }

    blocked = Ls_ReadRaw(h);
    if (h->ActiveLow != 0U) {
        blocked = Ls_Invert(blocked);       /* 低有效 -> 1 表示遮挡 */
    }

    for (i = 0U; i < LS_CHANNELS; i++) {
        uint8_t level = (uint8_t)((blocked >> i) & 1U);
        uint8_t state = (uint8_t)((h->State >> i) & 1U);

        if (level == state) {
            h->Count[i] = 0U;               /* 和当前状态一致, 计数清零 */
            continue;
        }

        if (h->Count[i] < 0xFFU) {
            h->Count[i]++;
        }

        if (h->Count[i] >= h->Debounce) {
            h->State ^= (uint8_t)(1U << i);
            h->Count[i] = 0U;
            changed |= (uint8_t)(1U << i);
        }
    }

    if (changed != 0U) {
        h->Changes++;
    }

    if (changedMask != NULL) {
        *changedMask = changed;
    }

    return h->State;
}

uint8_t Ls_GetState(const Ls_HandleTypeDef *h)
{
    return (h != NULL) ? (uint8_t)(h->State & LS_MASK) : 0U;
}

uint8_t Ls_GetRaw(const Ls_HandleTypeDef *h)
{
    return (h != NULL) ? (uint8_t)(h->Raw & LS_MASK) : 0U;
}

uint32_t Ls_GetScans(const Ls_HandleTypeDef *h)
{
    return (h != NULL) ? h->Scans : 0U;
}

void Ls_FormatMask(uint8_t mask, char *dst, uint16_t size)
{
    uint16_t pos = 0U;
    uint8_t  i;
    uint8_t  first = 1U;

    if ((dst == NULL) || (size == 0U)) {
        return;
    }

    mask &= LS_MASK;
    dst[0] = '\0';

    if (mask == 0U) {
        dst[0] = '-';
        if (size > 1U) {
            dst[1] = '\0';
        }
        return;
    }

    for (i = 0U; i < LS_CHANNELS; i++) {
        if (((mask >> i) & 1U) == 0U) {
            continue;
        }

        if ((uint16_t)(pos + 3U) >= size) {
            break;
        }

        if (first == 0U) {
            dst[pos++] = ',';
        }
        dst[pos++] = (char)('0' + i);
        first = 0U;
    }

    dst[pos] = '\0';
}

void Ls_PrintStatus(const Ls_HandleTypeDef *h)
{
    char text[LS_CHANNELS * 2U + 1U];

    if (h == NULL) {
        return;
    }

    Ls_FormatMask(Ls_GetState(h), text, (uint16_t)sizeof(text));

    LOG_I("state 0x%02X blocked[%s] raw 0x%02X scans %u changes %u\r\n",
          (unsigned)Ls_GetState(h),
          text,
          (unsigned)Ls_GetRaw(h),
          (unsigned)h->Scans,
          (unsigned)h->Changes);
}
