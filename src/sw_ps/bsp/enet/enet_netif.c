/**
 *******************************************************************************
 * @file    enet_netif.c
 * @brief   Ethernet module — lwIP network-interface service implementation
 *
 * Owns the struct netif of the handle and the address state machine.  The
 * DHCP wait is expressed as a tick counter instead of the blocking
 * "while (ip == 0) xemacif_input()" loop of the Xilinx example: the super-loop
 * keeps servicing the MAC, the protocol timers and the TCP services while the
 * lease is negotiated, and a bounded timeout falls back to the static address.
 *
 * @note   lwIP is configured with LWIP_IPV6 = 0, so ip_addr_t and ip4_addr_t
 *         are the same type and the conversions performed here are no-ops.
 *******************************************************************************
 */

#include "enet_netif.h"

#include <string.h>

#include "lwip/init.h"
#include "lwip/dhcp.h"
/* log tag: LOG_I/W/E from this file print with the [enet] prefix */
#define LOG_ENABLE 0   /* 本模块日志默认关闭; 需要时改成 1 */
#define LOG_TAG "enet"
#include "service/monitor_module/log/log.h"

#include "xil_printf.h"

#include "netif/xadapter.h"

/* ========================================================================== */
/*  内部函数(static)                                                              */
/* ========================================================================== */

/**
 * @brief  Program the static address carried by the configuration.
 * @param  h : Attached handle.
 */
static void Enet_Netif_ApplyStaticAddress(Enet_HandleTypeDef *h)
{
    netif_set_addr(&h->Netif,
                   &h->Config.StaticIp,
                   &h->Config.StaticNetmask,
                   &h->Config.StaticGateway);
}

/**
 * @brief  Append the decimal representation of a value to a buffer.
 * @param  dest  : Destination buffer.
 * @param  size  : Size of @p dest.
 * @param  pos   : Current write position, updated on return.
 * @param  value : Value to append (0 .. 255 in this module).
 *
 * Implemented locally so that the module does not pull the newlib formatted
 * output machinery into the image.
 */
static void Enet_Netif_AppendUint(char *dest, size_t size, size_t *pos, uint32_t value)
{
    char    digits[10];
    size_t  count = 0U;

    if (value == 0U) {
        digits[count++] = '0';
    }

    while ((value > 0U) && (count < sizeof(digits))) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }

    while ((count > 0U) && (*pos + 1U < size)) {
        dest[*pos] = digits[--count];
        (*pos)++;
    }

    dest[*pos] = '\0';
}

/* ========================================================================== */
/*  对外接口                                                                      */
/* ========================================================================== */

Enet_StatusTypeDef Enet_Netif_Attach(Enet_HandleTypeDef *h)
{
    ip4_addr_t zero;
    ip4_addr_t *ip;
    ip4_addr_t *mask;
    ip4_addr_t *gw;

    if (h == NULL) {
        return ENET_ERR_PARAM;
    }

    /* lwIP keeps a single global state; with NO_SYS = 1 this also resets the
     * protocol timers and the memory pools.
     */
    lwip_init();

    if (h->Config.UseDhcp != 0U) {
        /* With DHCP the interface starts on 0.0.0.0. */
        ip4_addr_set_zero(&zero);
        ip   = &zero;
        mask = &zero;
        gw   = &zero;
    } else {
        ip   = &h->Config.StaticIp;
        mask = &h->Config.StaticNetmask;
        gw   = &h->Config.StaticGateway;
    }

    if (xemac_add(&h->Netif, (ip_addr_t *)ip, (ip_addr_t *)mask, (ip_addr_t *)gw,
                  h->Config.MacAddress.Addr, h->Config.EmacBaseAddr) == NULL) {
        LOG_I("xemac_add() failed for MAC 0x%08X\r\n",
                   (unsigned)h->Config.EmacBaseAddr);
        return ENET_ERR_NETIF;
    }

    netif_set_default(&h->Netif);

    return ENET_OK;
}

Enet_StatusTypeDef Enet_Netif_Start(Enet_HandleTypeDef *h)
{
    if (h == NULL) {
        return ENET_ERR_PARAM;
    }

    netif_set_up(&h->Netif);

    if (h->Config.UseDhcp != 0U) {
        h->DhcpTimeoutCnt = h->Config.DhcpTimeoutTicks;

        if (dhcp_start(&h->Netif) != ERR_OK) {
            LOG_I("dhcp_start() failed\r\n");
            return ENET_ERR_DHCP;
        }

        {
            char fallback[16];

            Enet_Netif_FormatIp(&h->Config.StaticIp, fallback, sizeof(fallback));
            LOG_I("waiting for a DHCP lease (timeout %u ms); static fall-back %s\r\n",
                  (unsigned)(h->Config.DhcpTimeoutTicks * 500U), fallback);
        }
        return ENET_OK;                     /* address applied by Service()    */
    }

    Enet_Netif_ApplyStaticAddress(h);
    Enet_Netif_PrintAddresses(h);

    return ENET_OK;
}

Enet_StatusTypeDef Enet_Netif_Service(Enet_HandleTypeDef *h)
{
    if ((h == NULL) || (h->State != ENET_STATE_DHCP_WAIT)) {
        return ENET_OK;                     /* nothing to do                   */
    }

    /* ---- Lease arrived --------------------------------------------------- */
    if (h->Netif.ip_addr.addr != 0U) {
        h->Stats.DhcpLeases++;
        h->State = ENET_STATE_READY;

        LOG_I("DHCP lease acquired\r\n");
        Enet_Netif_PrintAddresses(h);
        return ENET_OK;
    }

    /* ---- Lease timeout --------------------------------------------------- */
    if (h->DhcpTimeoutCnt > 0U) {
        h->DhcpTimeoutCnt--;
        return ENET_OK;
    }

    h->Stats.DhcpTimeouts++;

    if (h->Config.DhcpFallbackToStatic == 0U) {
        LOG_I("DHCP timeout and no static fall-back configured\r\n");
        h->State = ENET_STATE_ERROR;
        return ENET_ERR_DHCP;
    }

    LOG_I("DHCP timeout, falling back to the static address\r\n");
    dhcp_stop(&h->Netif);
    Enet_Netif_ApplyStaticAddress(h);
    Enet_Netif_PrintAddresses(h);
    h->State = ENET_STATE_READY;

    return ENET_OK;
}

uint32_t Enet_Netif_Input(Enet_HandleTypeDef *h)
{
    uint32_t processed = 0U;
    uint32_t pass;
    int      packets;

    if ((h == NULL) || (h->Netif.state == NULL)) {
        return 0U;
    }

    /* Drain what the MAC has queued, but bound the work per pass so that an
     * endless receive burst cannot starve the timers and the TCP services.
     */
    for (pass = 0U; pass < ENET_RX_DRAIN_MAX; pass++) {
        packets = xemacif_input(&h->Netif);
        if (packets <= 0) {
            break;
        }
        processed += (uint32_t)packets;
    }

    h->Stats.RxPackets += processed;

    return processed;
}

void Enet_Netif_FormatIp(const ip4_addr_t *addr, char *buffer, size_t size)
{
    size_t pos = 0U;

    if ((addr == NULL) || (buffer == NULL) || (size == 0U)) {
        return;
    }

    buffer[0] = '\0';

    Enet_Netif_AppendUint(buffer, size, &pos, (uint32_t)ip4_addr1(addr));
    if (pos + 1U < size) { buffer[pos++] = '.'; buffer[pos] = '\0'; }
    Enet_Netif_AppendUint(buffer, size, &pos, (uint32_t)ip4_addr2(addr));
    if (pos + 1U < size) { buffer[pos++] = '.'; buffer[pos] = '\0'; }
    Enet_Netif_AppendUint(buffer, size, &pos, (uint32_t)ip4_addr3(addr));
    if (pos + 1U < size) { buffer[pos++] = '.'; buffer[pos] = '\0'; }
    Enet_Netif_AppendUint(buffer, size, &pos, (uint32_t)ip4_addr4(addr));
}

void Enet_Netif_PrintAddresses(const Enet_HandleTypeDef *h)
{
    char ip[16];
    char mask[16];
    char gw[16];

    if (h == NULL) {
        return;
    }

    Enet_Netif_FormatIp(&h->Netif.ip_addr, ip, sizeof(ip));
    Enet_Netif_FormatIp(&h->Netif.netmask, mask, sizeof(mask));
    Enet_Netif_FormatIp(&h->Netif.gw, gw, sizeof(gw));

    LOG_I("Board IP: %s\r\n", ip);
    LOG_I("Netmask : %s\r\n", mask);
    LOG_I("Gateway : %s\r\n", gw);
}
