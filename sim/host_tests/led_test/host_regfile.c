/**
 * 宿主机测试用: breath_led IP 的 AXI 寄存器文件替身 + 日志/回复桩。
 *
 * 寄存器窗口按真实 IP 建一个 64 KB 数组(pl.dtsi 里 reg = <0x43c00000 0x10000>),
 * CTRL / PERIOD 都是普通的读写寄存器, 所以这里就是一块**不加工**的存储:
 *
 *   !! 驱动写什么就存什么, 不做任何夹取/截断。IP 的夹取行为(10..1000)和
 *      PERIOD 只有 10 位这两件事, 必须由驱动自己保证 —— 测试要能看见驱动写出的
 *      原始值, 否则寄存器文件替身会把驱动的 bug 藏起来(比如驱动忘了夹取, 写个
 *      5000 进去, 硬件只会留下低 10 位, 而替身要如实记录 5000 让测试断言失败)。
 */
#include "xil_io.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp/led/led.h"

static uint8_t  sReg[HOST_LED_WINDOW];
static uint32_t sWrites;

u32 Xil_In32(UINTPTR Addr)
{
    uint32_t offset = (uint32_t)(Addr & (HOST_LED_WINDOW - 1U));

    if ((offset & 3U) != 0U) {
        return 0U;
    }

    return (uint32_t)sReg[offset] | ((uint32_t)sReg[offset + 1U] << 8) |
           ((uint32_t)sReg[offset + 2U] << 16) | ((uint32_t)sReg[offset + 3U] << 24);
}

void Xil_Out32(UINTPTR Addr, u32 Value)
{
    uint32_t offset = (uint32_t)(Addr & (HOST_LED_WINDOW - 1U));

    if ((offset & 3U) != 0U) {
        return;
    }

    sReg[offset]      = (uint8_t)(Value & 0xFFU);
    sReg[offset + 1U] = (uint8_t)((Value >> 8) & 0xFFU);
    sReg[offset + 2U] = (uint8_t)((Value >> 16) & 0xFFU);
    sReg[offset + 3U] = (uint8_t)((Value >> 24) & 0xFFU);
    sWrites++;
}

uint32_t HostReg_Read(uint32_t offset)
{
    return Xil_In32((UINTPTR)(LED_BASEADDR + offset));
}

void HostReg_Write(uint32_t offset, uint32_t value)
{
    Xil_Out32((UINTPTR)(LED_BASEADDR + offset), value);
}

void HostReg_Reset(void)
{
    (void)memset(sReg, 0, sizeof(sReg));
    sWrites = 0U;
}

uint32_t HostReg_Writes(void) { return sWrites; }

/* ========================================================================== */
/*  日志桩                                                                     */
/* ========================================================================== */

/* led.c / service_led.c 都是 LOG_ENABLE 0, 这些桩其实不会被调到; 留着是因为
 * 一旦有人把某个文件的 LOG_ENABLE 改成 1, 测试还能链接过并看到输出。 */
void     Log_Init(void) { }
void     Log_Lock(void) { }
void     Log_Unlock(void) { }
uint32_t Log_UptimeMs(void) { return 0U; }
void     Log_Banner(const char *title) { (void)title; }

void xil_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (void)vprintf(fmt, ap);
    va_end(ap);
}
