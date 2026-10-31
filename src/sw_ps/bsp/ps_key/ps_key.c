/**
 *******************************************************************************
 * @file    ps_key.c
 * @brief   PS 端按键(MIO10 / MIO11)读取 —— XGpioPs 薄封装
 *
 * 只做三件事: 初始化、配方向、读电平。**消抖和"按一下做什么"都不在这里** ——
 * 那是 service/inter_module 的事(bsp 只负责"把引脚读出来")。
 *
 * 关于 SDT 与 LookupConfig 的坑, 见 ps_key.h 顶部的说明 —— 本文件用的是
 * **基地址**版本 `XGpioPs_LookupConfig(XPAR_XGPIOPS_0_BASEADDR)`。
 *******************************************************************************
 */

#define LOG_ENABLE 1
#define LOG_TAG "key"
#include "service/monitor_module/log/log.h"

#include "bsp/ps_key/ps_key.h"

#include "xgpiops.h"
#include "xparameters.h"

/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

static XGpioPs gKey;
static uint8_t gReady;

/** @brief  把全局 MIO 号转成一个字的位掩码(读回来的值用 & 取位时方便)。 */
#define PS_KEY_BIT(Mio)   ((u32)1U << (Mio))

PsKey_StatusTypeDef PsKey_Init(void)
{
    XGpioPs_Config *cfg;

    /*
     * SDT 模式下按**基地址**找配置(不是 DeviceId)。
     * 这里传 XPAR_XGPIOPS_0_BASEADDR = 0xE000A000(导出的 xparameters.h 里给的)。
     */
    cfg = XGpioPs_LookupConfig((u32)XPAR_XGPIOPS_0_BASEADDR);
    if (cfg == NULL) {
        LOG_E("XGpioPs_LookupConfig(0x%08X) failed - PS GPIO not in this BSP?\r\n",
              (unsigned)XPAR_XGPIOPS_0_BASEADDR);
        return PS_KEY_ERR_HW;
    }

    if (XGpioPs_CfgInitialize(&gKey, cfg, cfg->BaseAddr) != XST_SUCCESS) {
        LOG_E("XGpioPs_CfgInitialize failed\r\n");
        return PS_KEY_ERR_HW;
    }

    /* 方向 0 = 输入。两个按键都是输入, 只配方向 —— 上拉是 ps7_init 配好的。 */
    XGpioPs_SetDirectionPin(&gKey, (u32)PS_KEY_MIO_KEY1, 0U);
    XGpioPs_SetDirectionPin(&gKey, (u32)PS_KEY_MIO_KEY2, 0U);

    gReady = 1U;

    LOG_I("PS keys ready: KEY1=MIO%u (ball E9), KEY2=MIO%u (ball C6), in=%u/%u\r\n",
          (unsigned)PS_KEY_MIO_KEY1, (unsigned)PS_KEY_MIO_KEY2,
          (unsigned)XGpioPs_ReadPin(&gKey, (u32)PS_KEY_MIO_KEY1),
          (unsigned)XGpioPs_ReadPin(&gKey, (u32)PS_KEY_MIO_KEY2));

    return PS_KEY_OK;
}

uint8_t PsKey_Ready(void)
{
    return gReady;
}

uint8_t PsKey_ReadLevel(uint32_t Mio)
{
    if ((gReady == 0U) || (Mio >= 32U)) {
        /*
         * 没初始化好、或者参数不对, 一律回 **1**(= 没按下)。
         * 按键的另一端接下来会接到"复位"这种要命的动作上, 所以读不到的时候必须偏
         * 安全侧 —— 宁可漏一次按不动, 也不能因为读了个假 0 就把板子复位了。
         */
        return 1U;
    }

    return (uint8_t)(XGpioPs_ReadPin(&gKey, Mio) & 0x1U);
}
