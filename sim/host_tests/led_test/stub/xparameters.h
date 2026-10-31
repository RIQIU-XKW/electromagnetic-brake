/**
 * 宿主机测试用: xparameters.h 的替身。
 *
 * 这里**故意定义** XPAR_BREATH_LED_0_BASEADDR, 因为 bsp/led/led.h 是
 *
 *     #define LED_BASEADDR   XPAR_BREATH_LED_0_BASEADDR
 *
 * 没有兜底分支 —— 真实平台 BSP 一定会给这个宏, 所以测试里也必须给, 否则编不过。
 * 地址取自 Vitis 导出的 xparameters.h / pl.dtsi:
 *     breath_led@43c00000 { reg = <0x43c00000 0x10000>; }
 */
#ifndef XPARAMETERS_H
#define XPARAMETERS_H

#define XPAR_BREATH_LED_0_BASEADDR  0x43C00000U
#define XPAR_BREATH_LED_0_HIGHADDR  0x43C0FFFFU

#endif /* XPARAMETERS_H */
