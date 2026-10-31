/**
 *******************************************************************************
 * @file    enet_phy.c
 * @brief   Ethernet module — PHY / GEM0 reference-clock layer implementation
 *
 * Why this file exists
 * --------------------
 * The Zynq-7000 GEM needs a reference clock that matches the negotiated link
 * speed.  The lwIP port normally programs it in SetUpSLCRDivisors(), but that
 * function is compiled out when the BSP is built for the System Device Tree
 * flow (-DSDT), so the value left behind by ps7_init() - i.e. whatever the
 * Vivado PS7 configuration of the hardware design produced - is what the MAC
 * actually runs on.
 *
 * Two failure modes have been observed on this board:
 *
 *   1. ENET0 "Clock Source" set to *External* in the PS7 configuration while
 *      the design does not enable ENET0 EMIO and the PL feeds no clock to
 *      GEM0_CLK.  GEM0_CLK_CTRL then selects the EMIO source, the MAC has no
 *      reference clock at all and the board is invisible on the network while
 *      the console still looks perfectly healthy.
 *   2. The design's divisors left at DIV0 = 1 (i.e. 1000 MHz from the IO PLL
 *      instead of 125 MHz), which makes the MAC clock data out eight times
 *      too fast: frames reach the PHY as garbage.
 *
 * Enet_Phy_ApplyClock() therefore verifies the register instead of trusting
 * it, repairs both conditions, and reports what it programmed.
 *******************************************************************************
 */

#include "enet_phy.h"

#include "xparameters.h"
#include "xparameters_ps.h"     /* XPS_SYS_CTRL_BASEADDR */
#include "xil_io.h"
/* log tag: LOG_I/W/E from this file print with the [enet] prefix */
#define LOG_ENABLE 0   /* 本模块日志默认关闭; 需要时改成 1 */
#define LOG_TAG "enet"
#include "service/monitor_module/log/log.h"

#include "xil_printf.h"

#include "netif/xadapter.h"
#include "netif/xemacpsif.h"

/* ========================================================================== */
/*  Zynq-7000 SLCR register map (GEM0 clock tree)                             */
/* ========================================================================== */

#define ENET_SLCR_UNLOCK_OFFSET     0x00000008U   /**< SLCR_UNLOCK              */
#define ENET_SLCR_LOCK_OFFSET       0x00000004U   /**< SLCR_LOCK                */
#define ENET_SLCR_GEM0_RCLK_CTRL    0x00000138U   /**< GEM0 Rx clock control    */
#define ENET_SLCR_GEM0_CLK_CTRL     0x00000140U   /**< GEM0 reference clock     */

#define ENET_SLCR_UNLOCK_KEY        0x0000DF0DU
#define ENET_SLCR_LOCK_KEY          0x0000767BU

/** GEM0_CLK_CTRL[6:4] : 00x IO PLL, 010 ARM PLL, 011 DDR PLL, 1xx EMIO. */
#define ENET_GEM_CLK_SRCSEL_MASK    0x00000070U
#define ENET_GEM_CLK_SRCSEL_EMIO    0x00000040U

/** GEM0_CLK_CTRL divisor fields. */
#define ENET_GEM_CLK_DIV0_SHIFT     8U
#define ENET_GEM_CLK_DIV1_SHIFT     20U

/** Keeps CLKACT (bit 0) and SRCSEL[6:4], clears DIV0[13:8] and DIV1[25:20]. */
#define ENET_GEM_CLK_DIV_KEEP_MASK  0xFC0FC0FFU

/** GEM0_RCLK_CTRL[4] : 0 = Rx clock/control/data from MIO, 1 = from EMIO. */
#define ENET_GEM_RCLK_SRCSEL_EMIO   0x00000010U

/* ========================================================================== */
/*  内部函数(static)                                                              */
/* ========================================================================== */

/**
 * @brief  Reach the EmacPs instance that backs a lwIP netif.
 * @param  netif : Interface created by xemac_add().
 * @return Pointer to the port's private structure, or NULL when not attached.
 *
 * netif->state -> struct xemac_s -> xemacpsif_s is the layout published by the
 * lwIP port headers, so the indirection is safe to use from application code.
 */
static xemacpsif_s *Enet_Phy_GetEmacPs(const struct netif *netif)
{
    struct xemac_s *xemac;

    if ((netif == NULL) || (netif->state == NULL)) {
        return NULL;
    }

    xemac = (struct xemac_s *)netif->state;
    if (xemac->state == NULL) {
        return NULL;
    }

    return (xemacpsif_s *)xemac->state;
}

/**
 * @brief  Write the GEM0 reference-clock control register.
 * @param  value : Value to program (SLCR is unlocked around the write).
 */
static void Enet_Phy_WriteGemClkCtrl(uint32_t value)
{
    Xil_Out32(XPS_SYS_CTRL_BASEADDR + ENET_SLCR_UNLOCK_OFFSET, ENET_SLCR_UNLOCK_KEY);
    Xil_Out32(XPS_SYS_CTRL_BASEADDR + ENET_SLCR_GEM0_CLK_CTRL, value);
    Xil_Out32(XPS_SYS_CTRL_BASEADDR + ENET_SLCR_LOCK_OFFSET, ENET_SLCR_LOCK_KEY);
}

/**
 * @brief  Ask the port to poll the PHY and refresh its link status.
 * @param  h : Initialised handle.
 */
static void Enet_Phy_PollPhy(Enet_HandleTypeDef *h)
{
    if (h->Netif.state != NULL) {
        eth_link_detect(&h->Netif);
    }
}

/* ========================================================================== */
/*  对外接口                                                                      */
/* ========================================================================== */

uint32_t Enet_Phy_GetDivisor1(Enet_SpeedTypeDef speed)
{
    uint32_t divisor;

    switch (speed) {
        case ENET_SPEED_1000: divisor = 1U;  break;   /* 1000/8/1  = 125 MHz  */
        case ENET_SPEED_100:  divisor = 5U;  break;   /* 1000/8/5  =  25 MHz  */
        case ENET_SPEED_10:   divisor = 50U; break;   /* 1000/8/50 = 2.5 MHz  */
        default:              divisor = 1U;  break;
    }

    return divisor;
}

Enet_SpeedTypeDef Enet_Phy_ReadSpeed(const Enet_HandleTypeDef *h)
{
    xemacpsif_s *emacps;
    uint16_t     speed;

    if (h == NULL) {
        return ENET_SPEED_UNKNOWN;
    }

    emacps = Enet_Phy_GetEmacPs(&h->Netif);
    if (emacps == NULL) {
        return ENET_SPEED_UNKNOWN;
    }

    speed = XEmacPs_GetOperatingSpeed(&emacps->emacps);

    switch (speed) {
        case 1000U: return ENET_SPEED_1000;
        case 100U:  return ENET_SPEED_100;
        case 10U:   return ENET_SPEED_10;
        default:    return ENET_SPEED_UNKNOWN;
    }
}

Enet_StatusTypeDef Enet_Phy_ApplyClock(Enet_HandleTypeDef *h)
{
    uint32_t ctrl;
    uint32_t target;
    uint32_t div0;
    uint32_t div1;
    uint32_t rclk;
    uint32_t srccel;

    if (h == NULL) {
        return ENET_ERR_PARAM;
    }

    div0 = (h->Config.ClockDiv0 != 0U) ? h->Config.ClockDiv0 : ENET_GEM_CLK_DIV0_IO_PLL;
    div1 = Enet_Phy_GetDivisor1(h->Link.Speed);

    ctrl   = Xil_In32(XPS_SYS_CTRL_BASEADDR + ENET_SLCR_GEM0_CLK_CTRL);
    srccel = ctrl & ENET_GEM_CLK_SRCSEL_MASK;

    /* ---- Source selection policy ----------------------------------------- */
    if (h->Config.ClockSource == ENET_CLKSRC_EXTERNAL_EMIO) {
        /* The PL drives GEM0_CLK; the MAC clock is not ours to program. */
        LOG_I("GEM0 reference clock: external (EMIO), left untouched\r\n");
        return ENET_OK;
    }

    if (h->Config.ClockSource == ENET_CLKSRC_IO_PLL) {
        ctrl &= ~ENET_GEM_CLK_SRCSEL_MASK;              /* force the IO PLL      */
    } else if ((srccel & ENET_GEM_CLK_SRCSEL_EMIO) != 0U) {
        /* ENET_CLKSRC_AUTO and the design asks the PL for a clock that this
         * board does not feed: repair it, otherwise GEM0 stays clock-less.
         */
        LOG_I("GEM0 clock source was the PL (EMIO) but no EMIO clock is "
                   "wired; switching to the IO PLL\r\n");
        ctrl &= ~ENET_GEM_CLK_SRCSEL_MASK;
    } else {
        /* ENET_CLKSRC_AUTO with a usable PLL source: keep it, verify divisors. */
    }

    /* ---- Divisors -------------------------------------------------------- */
    ctrl &= ENET_GEM_CLK_DIV_KEEP_MASK;                 /* keep CLKACT + source   */
    ctrl |= (div0 << ENET_GEM_CLK_DIV0_SHIFT);
    ctrl |= (div1 << ENET_GEM_CLK_DIV1_SHIFT);
    target = ctrl;

    /* ---- GEM0 Rx clock must follow the same path as the RGMII pins ------ */
    rclk = Xil_In32(XPS_SYS_CTRL_BASEADDR + ENET_SLCR_GEM0_RCLK_CTRL);

    if ((target != Xil_In32(XPS_SYS_CTRL_BASEADDR + ENET_SLCR_GEM0_CLK_CTRL)) ||
        ((rclk & ENET_GEM_RCLK_SRCSEL_EMIO) != 0U)) {

        Enet_Phy_WriteGemClkCtrl(target);

        if ((rclk & ENET_GEM_RCLK_SRCSEL_EMIO) != 0U) {
            Xil_Out32(XPS_SYS_CTRL_BASEADDR + ENET_SLCR_UNLOCK_OFFSET, ENET_SLCR_UNLOCK_KEY);
            Xil_Out32(XPS_SYS_CTRL_BASEADDR + ENET_SLCR_GEM0_RCLK_CTRL,
                      rclk & ~ENET_GEM_RCLK_SRCSEL_EMIO);
            Xil_Out32(XPS_SYS_CTRL_BASEADDR + ENET_SLCR_LOCK_OFFSET, ENET_SLCR_LOCK_KEY);
        }

        h->Stats.ClockFixups++;
    }

    /* ---- Report and verify ---------------------------------------------- */
    ctrl = Xil_In32(XPS_SYS_CTRL_BASEADDR + ENET_SLCR_GEM0_CLK_CTRL);

    LOG_I("GEM0 reference clock: %u Mbps, ctrl = 0x%08X "
               "(source = %s, DIV0 = %u, DIV1 = %u)\r\n",
               (unsigned)h->Link.Speed,
               (unsigned)ctrl,
               ((ctrl & ENET_GEM_CLK_SRCSEL_MASK) == ENET_GEM_CLK_SRCSEL_EMIO) ? "EMIO" : "PLL",
               (unsigned)((ctrl >> ENET_GEM_CLK_DIV0_SHIFT) & 0x3FU),
               (unsigned)((ctrl >> ENET_GEM_CLK_DIV1_SHIFT) & 0x3FU));

    if (ctrl != target) {
        LOG_I("WARNING: GEM0 clock read-back mismatch (wrote 0x%08X)\r\n",
                   (unsigned)target);
        return ENET_ERR_HW;
    }

    return ENET_OK;
}

Enet_StatusTypeDef Enet_Phy_UpdateLink(Enet_HandleTypeDef *h)
{
    xemacpsif_s          *emacps;
    Enet_LinkStatusTypeDef status;
    Enet_SpeedTypeDef      speed;
    uint8_t                changed = 0U;

    if (h == NULL) {
        return ENET_ERR_PARAM;
    }

    emacps = Enet_Phy_GetEmacPs(&h->Netif);
    if (emacps == NULL) {
        return ENET_ERR_STATE;
    }

    Enet_Phy_PollPhy(h);

    /* ---- Link status ----------------------------------------------------- */
    status = (Enet_LinkStatusTypeDef)emacps->eth_link_status;
    if (status != h->Link.Status) {
        h->Link.Status = status;
        changed = 1U;

        if (status == ENET_LINK_UP) {
            h->Link.Up = 1U;
            h->Link.UpCount++;
        } else if (h->Link.Up != 0U) {
            h->Link.Up = 0U;
            h->Link.DownCount++;
        }
    }

    /* ---- Speed ----------------------------------------------------------- */
    speed = Enet_Phy_ReadSpeed(h);
    if ((speed != ENET_SPEED_UNKNOWN) && (speed != h->Link.Speed)) {
        if (h->Link.Speed != ENET_SPEED_UNKNOWN) {
            /* The reference clock has to follow a renegotiated rate, otherwise
             * the MAC would clock the PHY at the speed of the previous link.
             */
            h->Link.SpeedChanges++;
        }

        h->Link.Speed = speed;
        changed = 1U;

        if ((h->Link.SpeedChanges != 0U) && (Enet_Phy_ApplyClock(h) == ENET_OK)) {
            h->Stats.PhySpeedFixups++;
        }
    }

    /* ---- Report ---------------------------------------------------------- */
    if (changed != 0U) {
        LOG_I("link %s, speed %u Mbps%s\r\n",
                   Enet_Phy_LinkStatusName(h->Link.Status),
                   (unsigned)h->Link.Speed,
                   (h->Link.SpeedChanges != 0U) ? " (renegotiated)" : "");

        if (h->OnLinkChange != NULL) {
            h->OnLinkChange(h->LinkContext, &h->Link);
        }
    }

    return ENET_OK;
}

void Enet_Phy_RecoverRxPath(Enet_HandleTypeDef *h)
{
    if (h == NULL) {
        return;
    }

    if (h->Netif.state != NULL) {
        xemacpsif_resetrx_on_no_rxdata(&h->Netif);
    }
}

const char *Enet_Phy_LinkStatusName(Enet_LinkStatusTypeDef status)
{
    switch (status) {
        case ENET_LINK_UP:          return "up";
        case ENET_LINK_DOWN:        return "down";
        case ENET_LINK_NEGOTIATING: return "negotiating";
        case ENET_LINK_UNDEFINED:   /* fall through */
        default:                    return "unknown";
    }
}
