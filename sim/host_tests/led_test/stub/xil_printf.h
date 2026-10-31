/** 宿主机测试用: xil_printf.h 的替身(bsp/led/led.c 通过 log.h 用到)。 */
#ifndef XIL_PRINTF_H
#define XIL_PRINTF_H

void xil_printf(const char *fmt, ...);

#endif /* XIL_PRINTF_H */
