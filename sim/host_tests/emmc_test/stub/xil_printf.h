/** 宿主机测试用: xil_printf.h 的替身(log/log.h 会 include 它)。 */
#ifndef XIL_PRINTF_H
#define XIL_PRINTF_H

void xil_printf(const char *fmt, ...);

#endif /* XIL_PRINTF_H */
