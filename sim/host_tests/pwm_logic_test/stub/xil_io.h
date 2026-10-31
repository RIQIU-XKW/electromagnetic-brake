/** 宿主机测试用: xil_io.h / xil_types.h 的最小替身(AXI 寄存器读写)。 */
#ifndef XIL_IO_H
#define XIL_IO_H

#include <stdint.h>

typedef uint32_t  u32;
typedef int32_t   s32;
typedef uintptr_t UINTPTR;

u32  Xil_In32(UINTPTR Addr);
void Xil_Out32(UINTPTR Addr, u32 Value);

/* ---- 测试辅助: 寄存器文件访问 ---- */
#define HOST_PWM_WINDOW   0x10000U
uint32_t HostReg_Read(uint32_t offset);
void     HostReg_Write(uint32_t offset, uint32_t value);
void     HostReg_Reset(void);

#endif /* XIL_IO_H */
