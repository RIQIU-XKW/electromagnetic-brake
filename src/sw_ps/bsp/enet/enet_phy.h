/**
 *******************************************************************************
 * @file    enet_phy.h
 * @brief   PHY / GEM0 参考时钟层
 *
 * 管 MAC 到铜口之间的三件事:
 *   - Zynq-7000 SLCR 的 GEM0 时钟树(源选择 + 分频)。这是最容易让接口"静默死掉"
 *     的板级资源: MAC 照发, 但 PHY 拿到的时钟和协商速率差整数倍, 线上什么也没有;
 *   - 周期轮询链路/速率, 报告链路事件;
 *   - SI-692601 的 EmacPs 接收路径恢复规避。
 *
 * @note 参考时钟必须在 xemac_add() 之后(那时才知道 PHY 速率)、netif_set_up() 之前
 *       (第一次发送就在 set_up 之后)编程。
 *******************************************************************************
 */

#ifndef ENET_PHY_H
#define ENET_PHY_H

#include "enet_types.h"

/** SLCR GEM0 参考时钟源选择 = GEM0_CLK_CTRL 的 [6:4]。 */
typedef enum {
    ENET_GEM_CLK_SRC_IO_PLL  = 0x0U,    /**< 00x : IO PLL                   */
    ENET_GEM_CLK_SRC_ARM_PLL = 0x2U,    /**< 010 : ARM PLL                  */
    ENET_GEM_CLK_SRC_DDR_PLL = 0x3U,    /**< 011 : DDR PLL                  */
    ENET_GEM_CLK_SRC_EMIO    = 0x4U     /**< 1xx : 由 PL 提供               */
} Enet_GemClkSrcTypeDef;

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief  按当前链路速率编程 GEM0 参考时钟。
 *
 * 行为取决于配置里的 ClockSource:
 *   ENET_CLKSRC_AUTO          : 设计里选的源可用就保留; 选的是 EMIO 就切回内部
 *                               IO PLL(本板没给 GEM0 接 PL 时钟);
 *   ENET_CLKSRC_IO_PLL        : 一律强制内部 IO PLL 路径;
 *   ENET_CLKSRC_EXTERNAL_EMIO : 完全不碰寄存器。
 *
 * DIV0 来自配置, DIV1 按协商速率选(1000/100/10 Mbps -> 1/5/50)。
 * GEM0 的 Rx 时钟源同时强制走 MIO(RGMII): Rx 时钟、控制、数据必须同源。
 */
Enet_StatusTypeDef Enet_Phy_ApplyClock(Enet_HandleTypeDef *h);

/** @brief  轮询 PHY, 更新 h->Link 并报告状态变化。 */
Enet_StatusTypeDef Enet_Phy_UpdateLink(Enet_HandleTypeDef *h);

/** @brief  读 MAC 里当前编程的链路速率(读不到返回 ENET_SPEED_UNKNOWN)。 */
Enet_SpeedTypeDef Enet_Phy_ReadSpeed(const Enet_HandleTypeDef *h);

/** @brief  速率 -> GEM0 SLCR 的 DIV1(1 / 5 / 50; 未知速率返回 1)。 */
uint32_t Enet_Phy_GetDivisor1(Enet_SpeedTypeDef speed);

/** @brief  SI-692601 规避: 长时间没收到帧时复位接收路径。 */
void Enet_Phy_RecoverRxPath(Enet_HandleTypeDef *h);

/** @brief  链路状态的可读名字("up" / "down" ...)。 */
const char *Enet_Phy_LinkStatusName(Enet_LinkStatusTypeDef status);

#endif /* ENET_PHY_H */
