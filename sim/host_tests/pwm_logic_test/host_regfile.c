/**
 * 宿主机测试用: AXI 寄存器文件的替身 + 日志桩。
 *
 * 寄存器窗口按真实 IP 的行为建一个 64 KB 数组, 上电预置 ID 寄存器; 这样
 * 驱动的 "写进去再读回来" 自检、影子寄存器、位域打包都能被真实验证。
 */
#include "xil_io.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp/pwm/pwm.h"

static uint8_t sReg[HOST_PWM_WINDOW];
static uint32_t sWrites;

u32 Xil_In32(UINTPTR Addr)
{
    uint32_t offset = (uint32_t)(Addr & (HOST_PWM_WINDOW - 1U));

    if ((offset & 3U) != 0U) {
        return 0U;
    }

    /* STATUS 是只读的, 由硬件实时产生: 这里按 IP 手册的位定义现算出来,
     * 驱动的 Pwm_SelfTest() 正是靠 PERIOD_OK 这一位判断寄存器通路是否正常。 */
    if (offset == PWM_AXI_REG_STATUS) {
        uint32_t status = 0U;

        if ((Xil_In32((UINTPTR)(PWM_BASEADDR + PWM_AXI_REG_CTRL)) &
             PWM_AXI_CTRL_GLB_EN) != 0U) {
            status |= PWM_AXI_STATUS_GLB_EN;
        }
        if (Xil_In32((UINTPTR)(PWM_BASEADDR + PWM_AXI_REG_PERIOD)) != 0U) {
            status |= PWM_AXI_STATUS_PERIOD_OK;
        }
        status |= PWM_AXI_STATUS_CNT_ZERO;          /* 计数器停在 0 */
        if ((Xil_In32((UINTPTR)(PWM_BASEADDR + PWM_AXI_REG_PWM_EN)) &
             PWM_AXI_EN_MASK_ALL) != 0U) {
            status |= PWM_AXI_STATUS_ANY_EN;
        }
        return status;
    }

    return (uint32_t)sReg[offset] | ((uint32_t)sReg[offset + 1U] << 8) |
           ((uint32_t)sReg[offset + 2U] << 16) | ((uint32_t)sReg[offset + 3U] << 24);
}

void Xil_Out32(UINTPTR Addr, u32 Value)
{
    uint32_t offset = (uint32_t)(Addr & (HOST_PWM_WINDOW - 1U));

    if ((offset & 3U) != 0U) {
        return;
    }

    if ((offset == PWM_AXI_REG_STATUS) || (offset == PWM_AXI_REG_ID)) {
        return;                             /* 只读寄存器: 写入无效 */
    }

    sReg[offset]      = (uint8_t)(Value & 0xFFU);
    sReg[offset + 1U] = (uint8_t)((Value >> 8) & 0xFFU);
    sReg[offset + 2U] = (uint8_t)((Value >> 16) & 0xFFU);
    sReg[offset + 3U] = (uint8_t)((Value >> 24) & 0xFFU);
    sWrites++;
}

uint32_t HostReg_Read(uint32_t offset)
{
    return Xil_In32((UINTPTR)(PWM_BASEADDR + offset));
}

void HostReg_Write(uint32_t offset, uint32_t value)
{
    Xil_Out32((UINTPTR)(PWM_BASEADDR + offset), value);
}

void HostReg_Reset(void)
{
    (void)memset(sReg, 0, sizeof(sReg));
    sWrites = 0U;

    /* 上电就有的只读 ID 寄存器: 0x50574D31 = "PWM1"
     * (ID 寄存器是只读的, 所以这里直接写数组, 不走 Xil_Out32) */
    sReg[PWM_AXI_REG_ID + 0U] = (uint8_t)(PWM_AXI_ID_VALUE & 0xFFU);
    sReg[PWM_AXI_REG_ID + 1U] = (uint8_t)((PWM_AXI_ID_VALUE >> 8) & 0xFFU);
    sReg[PWM_AXI_REG_ID + 2U] = (uint8_t)((PWM_AXI_ID_VALUE >> 16) & 0xFFU);
    sReg[PWM_AXI_REG_ID + 3U] = (uint8_t)((PWM_AXI_ID_VALUE >> 24) & 0xFFU);
}

uint32_t HostReg_Writes(void) { return sWrites; }

/* ========================================================================== */
/*  日志桩: 驱动里新增的 Pwm_PrintStatus 用 LOG_I, 直接打到 stdout            */
/* ========================================================================== */

void Log_Init(void) { }
void Log_Lock(void) { }
void Log_Unlock(void) { }
uint32_t Log_UptimeMs(void) { return 0U; }
void Log_Banner(const char *title) { (void)title; }

void xil_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (void)vprintf(fmt, ap);
    va_end(ap);
}
