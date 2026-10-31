/**
 *******************************************************************************
 * @file    ps_rst.h
 * @brief   PS 软复位 —— 让整片 PS 重新走一遍上电初始化
 *
 * 往 SLCR 的 PSS_RST_CTRL 写 SOFT_RST, PS 就像刚上电那样从头起来:
 * boot ROM -> FSBL(重跑 ps7_init: 时钟/PLL/MIO/DDR/GPIO 全部重配) -> 应用。
 *
 * 上板前要知道:
 *   1. PL 的位流不会重新加载, 软复位只复位 PS 侧(要连 PL 一起重来只能断电);
 *   2. 复位后去哪儿看启动方式: 从 Flash 启动会自己重新加载 BOOT.bin;
 *      从 JTAG 启动会回到 boot ROM 等调试器, 板子看上去就"死"了 —— 开发阶段
 *      按这个键就是这个后果, 属于预期行为。
 *
 * 复位前要刷 cache: 复位会把 DDR 控制器一起重启, 没写回的数据就永远丢了。
 *******************************************************************************
 */

#ifndef BSP_PS_RST_H
#define BSP_PS_RST_H

#include <stdint.h>

/** @brief  复位 PS(不返回); 真返回了说明复位没生效, 调用者打日志继续跑。 */
void PsRst_SoftReset(void);

#endif /* BSP_PS_RST_H */
