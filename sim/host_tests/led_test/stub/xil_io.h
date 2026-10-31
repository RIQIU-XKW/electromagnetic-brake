/**
 * 宿主机测试用: xil_io.h / xil_types.h 的最小替身(AXI 寄存器读写)。
 *
 * 和 tools/pwm_logic_test/stub 里那份是同一个套路, 分开放是为了两个测试各自的
 * 窗口/宏互不干扰(它们编的是不同的驱动)。
 */
#ifndef XIL_IO_H
#define XIL_IO_H

#include <stdint.h>

typedef uint32_t  u32;
typedef int32_t   s32;
typedef uintptr_t UINTPTR;

u32  Xil_In32(UINTPTR Addr);
void Xil_Out32(UINTPTR Addr, u32 Value);

/* ---- 测试辅助: 寄存器文件访问 ---- */
#define HOST_LED_WINDOW   0x10000U      /* breath_led 的地址范围(pl.dtsi: 64 KB) */
uint32_t HostReg_Read(uint32_t offset);
void     HostReg_Write(uint32_t offset, uint32_t value);
void     HostReg_Reset(void);
uint32_t HostReg_Writes(void);

#endif /* XIL_IO_H */
