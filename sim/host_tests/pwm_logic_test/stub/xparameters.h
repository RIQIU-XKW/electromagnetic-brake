/**
 * 宿主机测试用: xparameters.h 桩。
 *
 * 故意**不**定义 XPAR_PWM_AXI_IP_0_BASEADDR, 这样 bsp/pwm/pwm.h 会走它的
 * 兜底分支(PWM_BASEADDR = 0x43C10000, 与 pl.dtsi 一致) —— 顺便验证那条分支
 * 是对的。平台 BSP 重建后真实工程会定义 XPAR, 走另一条分支。
 */
#ifndef XPARAMETERS_H
#define XPARAMETERS_H

#endif
