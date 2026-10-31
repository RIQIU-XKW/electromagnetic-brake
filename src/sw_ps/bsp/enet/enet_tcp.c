/**
 *******************************************************************************
 * @file    enet_tcp.c
 * @brief   Ethernet module — TCP service layer implementation (RAW API)
 *
 * The service keeps a fixed pool of connection objects inside the handle.
 * Every accepted connection is bound to one pool slot, and that slot pointer
 * is registered with lwIP through tcp_arg(), so each callback reaches its
 * connection state with a single cast instead of a look-up:
 *
 *     lwIP callback  --->  Enet_TcpChannelTypeDef  --->  Enet_HandleTypeDef
 *                          (Pcb, counters)              (Config, Stats, hooks)
 *
 * Payload handling is chain aware: a received pbuf may be a chain, so every
 * buffer is forwarded individually and the transfer is bounded by the sender
 * window (tcp_sndbuf()).  An application hook may claim the payload; when no
 * hook is installed (or the hook declines) the payload is echoed back.
 *******************************************************************************
 */

#define LOG_ENABLE 0   /* 本模块日志默认关闭; 需要时改成 1 */
#define LOG_TAG "enet"
#include "service/monitor_module/log/log.h"

#include "enet_tcp.h"
#include "enet_netif.h"

#include <string.h>

#include "lwip/err.h"
#include "lwip/tcp.h"
#include "xil_printf.h"

/* ========================================================================== */
/*  内部函数(static)                                                              */
/* ========================================================================== */

/**
 * @brief  Take the first free slot of the channel pool.
 * @param  h : Handle owning the pool.
 * @return Slot pointer, or NULL when every slot is busy.
 */
static Enet_TcpChannelTypeDef *Enet_Tcp_AllocChannel(Enet_HandleTypeDef *h)
{
    uint32_t i;

    for (i = 0U; i < ENET_TCP_MAX_CHANNELS; i++) {
        if (h->Channels[i].InUse == 0U) {
            Enet_TcpChannelTypeDef *channel = &h->Channels[i];

            (void)memset(channel, 0, sizeof(*channel));
            channel->InUse  = 1U;
            channel->Owner  = h;

            return channel;
        }
    }

    return NULL;
}

/**
 * @brief  Return a slot to the pool.
 * @param  channel : Slot to release (its pcb pointer is cleared).
 */
static void Enet_Tcp_ReleaseChannel(Enet_TcpChannelTypeDef *channel)
{
    if (channel == NULL) {
        return;
    }

    channel->Pcb   = NULL;
    channel->InUse = 0U;
}

/**
 * @brief  Print the peer address of a connection.
 * @param  pcb : Connection whose remote address is printed.
 */
static void Enet_Tcp_PrintPeer(const struct tcp_pcb *pcb)
{
    char buffer[16];

    Enet_Netif_FormatIp((const ip4_addr_t *)&pcb->remote_ip, buffer, sizeof(buffer));

    LOG_RAW("%s:%u", buffer, (unsigned)pcb->remote_port);
}

/* ========================================================================== */
/*  Payload forwarding                                                        */
/* ========================================================================== */

/**
 * @brief  Write a buffer to a connection and push it out.
 *
 * The send window is checked first so that a payload larger than the free
 * window is reported instead of being silently queued forever.  The data is
 * copied (TCP_WRITE_FLAG_COPY), which keeps the caller free to reuse its
 * buffer as soon as the call returns.
 *
 * @param  channel : Connection to write to.
 * @param  data    : Payload.
 * @param  len     : Payload length in bytes.
 * @return ENET_OK, ENET_ERR_PARAM, ENET_ERR_STATE, ENET_ERR_NO_MEMORY or
 *         ENET_ERR_HW.
 */
static Enet_StatusTypeDef Enet_Tcp_Write(Enet_TcpChannelTypeDef *channel,
                                         const void *data, uint16_t len)
{
    Enet_HandleTypeDef *h;

    if ((channel == NULL) || (data == NULL) || (len == 0U)) {
        return ENET_ERR_PARAM;
    }

    h = channel->Owner;

    if ((h == NULL) || (channel->Pcb == NULL)) {
        return ENET_ERR_STATE;
    }

    if (tcp_sndbuf(channel->Pcb) < len) {
        h->Stats.TcpErrors++;
        return ENET_ERR_NO_MEMORY;
    }

    if (tcp_write(channel->Pcb, data, len, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        h->Stats.TcpErrors++;
        return ENET_ERR_HW;
    }

    /* Push now instead of waiting for the next timer tick: an echo service
     * that batched its answers would look broken to a latency test.
     */
    (void)tcp_output(channel->Pcb);

    channel->TxBytes    += len;
    h->Stats.TcpTxBytes += len;

    return ENET_OK;
}

/**
 * @brief  Forward one contiguous received buffer.
 *
 * The application hook gets the first chance to consume the payload.  When it
 * declines - or when no hook is installed - the buffer is written back to the
 * sender, but only if the send window can take it; otherwise the segment is
 * dropped and counted, which is the graceful behaviour for an echo service.
 *
 * @param  channel : Connection that received the data.
 * @param  data    : Contiguous payload.
 * @param  len     : Payload length in bytes.
 * @return ENET_OK when the payload was consumed or echoed, otherwise the
 *         status of the failed write.
 */
static Enet_StatusTypeDef Enet_Tcp_Forward(Enet_TcpChannelTypeDef *channel,
                                           const void *data, uint16_t len)
{
    Enet_HandleTypeDef *h = channel->Owner;

    if (h == NULL) {
        return ENET_ERR_STATE;
    }

    /* ---- 1. application hook (command server, protocol handler, ...) ----- */
    if (h->OnTcpReceive != NULL) {
        if (h->OnTcpReceive(h->TcpContext, (const uint8_t *)data, len, channel) == ENET_OK) {
            return ENET_OK;
        }
    }

    /* ---- 2. default behaviour: echo the payload -------------------------- */
    return Enet_Tcp_Write(channel, data, len);
}

/* ========================================================================== */
/*  lwIP callbacks                                                            */
/* ========================================================================== */

static err_t Enet_Tcp_RecvCb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
static void  Enet_Tcp_ErrorCb(void *arg, err_t err);

/**
 * @brief  tcp_accept() callback: bind a pool slot to the new connection.
 * @param  arg    : Handle passed to tcp_arg() on the listening pcb.
 * @param  newpcb : Freshly accepted connection.
 * @param  err    : lwIP status of the connection.
 * @return ERR_OK, or ERR_MEM to let lwIP drop a connection we cannot track.
 */
static err_t Enet_Tcp_AcceptCb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    Enet_HandleTypeDef     *h       = (Enet_HandleTypeDef *)arg;
    Enet_TcpChannelTypeDef *channel = Enet_Tcp_AllocChannel(h);

    (void)err;

    if (channel == NULL) {
        /* No slot available: refuse the connection instead of leaking it. */
        h->Stats.TcpRejected++;
        LOG_W("TCP connection refused: channel pool exhausted\r\n");
        return ERR_MEM;
    }

    channel->Pcb = newpcb;

    tcp_arg(newpcb, channel);
    tcp_recv(newpcb, Enet_Tcp_RecvCb);
    tcp_err(newpcb, Enet_Tcp_ErrorCb);

    h->Stats.TcpAccepted++;

    /* 一行完整的接受日志: 用日志锁把几次写串口圈起来, 免得被别的任务切开. */
    Log_Lock();
    LOG_RAW("[enet] TCP connection %u accepted from ",
               (unsigned)h->Stats.TcpAccepted);
    Enet_Tcp_PrintPeer(newpcb);
    LOG_RAW("\r\n");
    Log_Unlock();

    return ERR_OK;
}

/**
 * @brief  tcp_recv() callback: forward the payload, or close on FIN.
 * @param  arg : Channel bound to this connection.
 * @param  pcb : Connection that received data.
 * @param  p   : Received pbuf chain, NULL when the peer closed the connection.
 * @param  err : lwIP status.
 * @return ERR_OK on success.
 */
static err_t Enet_Tcp_RecvCb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    Enet_TcpChannelTypeDef *channel = (Enet_TcpChannelTypeDef *)arg;
    Enet_HandleTypeDef     *h;
    struct pbuf            *segment;

    if (channel == NULL) {
        if (p != NULL) {
            (void)pbuf_free(p);
        }
        return ERR_VAL;
    }

    h = channel->Owner;

    /* ---- remote closed the connection (FIN) ------------------------------ */
    if (p == NULL) {
        if (h != NULL) {
            h->Stats.TcpClosed++;
        }

        Log_Lock();
        LOG_RAW("[enet] TCP connection closed");
        if ((channel->RxBytes != 0U) || (channel->TxBytes != 0U)) {
            LOG_RAW(" (%u bytes in, %u bytes out)",
                       (unsigned)channel->RxBytes, (unsigned)channel->TxBytes);
        }
        LOG_RAW("\r\n");
        Log_Unlock();

        (void)tcp_close(pcb);
        Enet_Tcp_ReleaseChannel(channel);
        return ERR_OK;
    }

    /* ---- protocol error -------------------------------------------------- */
    if (err != ERR_OK) {
        (void)pbuf_free(p);
        if (h != NULL) {
            h->Stats.TcpErrors++;
        }
        return err;
    }

    /* Acknowledge the whole chain so that the window keeps moving. */
    tcp_recved(pcb, p->tot_len);

    channel->RxSegments++;
    channel->RxBytes += p->tot_len;

    if (h != NULL) {
        h->Stats.TcpRxBytes += p->tot_len;
    }

    for (segment = p; segment != NULL; segment = segment->next) {
        if (segment->len == 0U) {
            continue;
        }

        if (Enet_Tcp_Forward(channel, segment->payload, segment->len) != ENET_OK) {
            break;                          /* window closed or write error    */
        }
    }

    (void)pbuf_free(p);

    return ERR_OK;
}

/**
 * @brief  tcp_err() callback: the connection was aborted by the stack.
 * @param  arg : Channel bound to this connection.
 * @param  err : Abort reason.
 *
 * @note   lwIP has already released the pcb here, so it must not be closed
 *         again - only the pool slot is given back.
 */
static void Enet_Tcp_ErrorCb(void *arg, err_t err)
{
    Enet_TcpChannelTypeDef *channel = (Enet_TcpChannelTypeDef *)arg;

    (void)err;

    if (channel == NULL) {
        return;
    }

    if (channel->Owner != NULL) {
        channel->Owner->Stats.TcpErrors++;
    }

    Enet_Tcp_ReleaseChannel(channel);
}

/* ========================================================================== */
/*  对外接口                                                                      */
/* ========================================================================== */

Enet_StatusTypeDef Enet_Tcp_Start(Enet_HandleTypeDef *h)
{
    struct tcp_pcb *pcb;
    err_t           err;

    if (h == NULL) {
        return ENET_ERR_PARAM;
    }

    if (h->ListenPcb != NULL) {
        return ENET_ERR_STATE;              /* already listening               */
    }

    pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb == NULL) {
        LOG_E("out of memory creating the TCP control block\r\n");
        return ENET_ERR_NO_MEMORY;
    }

    err = tcp_bind(pcb, IP_ANY_TYPE, h->Config.EchoPort);
    if (err != ERR_OK) {
        LOG_E("unable to bind port %u (err = %d)\r\n",
              (unsigned)h->Config.EchoPort, (int)err);
        (void)tcp_close(pcb);
        return ENET_ERR_BIND;
    }

    /* tcp_listen() frees the original pcb and returns the listening one. */
    pcb = tcp_listen(pcb);
    if (pcb == NULL) {
        LOG_E("out of memory while listening on port %u\r\n",
              (unsigned)h->Config.EchoPort);
        return ENET_ERR_NO_MEMORY;
    }

    tcp_arg(pcb, h);
    tcp_accept(pcb, Enet_Tcp_AcceptCb);

    h->ListenPcb = pcb;

    LOG_I("TCP service listening on port %u\r\n", (unsigned)h->Config.EchoPort);

    return ENET_OK;
}

void Enet_Tcp_Stop(Enet_HandleTypeDef *h)
{
    uint32_t i;

    if (h == NULL) {
        return;
    }

    for (i = 0U; i < ENET_TCP_MAX_CHANNELS; i++) {
        Enet_TcpChannelTypeDef *channel = &h->Channels[i];

        if (channel->InUse == 0U) {
            continue;
        }

        if (channel->Pcb != NULL) {
            tcp_arg(channel->Pcb, NULL);
            tcp_recv(channel->Pcb, NULL);
            tcp_err(channel->Pcb, NULL);
            (void)tcp_close(channel->Pcb);
        }

        Enet_Tcp_ReleaseChannel(channel);
    }

    if (h->ListenPcb != NULL) {
        tcp_arg(h->ListenPcb, NULL);
        tcp_accept(h->ListenPcb, NULL);
        (void)tcp_close(h->ListenPcb);
        h->ListenPcb = NULL;
    }
}

void Enet_Tcp_SetReceiveHook(Enet_HandleTypeDef *h,
                             Enet_TcpReceiveHook hook,
                             void *context)
{
    if (h == NULL) {
        return;
    }

    h->OnTcpReceive = hook;
    h->TcpContext   = context;
}

Enet_StatusTypeDef Enet_Tcp_Send(Enet_TcpChannelTypeDef *channel,
                                 const void *data, uint16_t len)
{
    return Enet_Tcp_Write(channel, data, len);
}

Enet_StatusTypeDef Enet_Tcp_Broadcast(Enet_HandleTypeDef *h,
                                      const void *data, uint16_t len)
{
    uint32_t failed = 0U;
    uint32_t i;

    if ((h == NULL) || (data == NULL) || (len == 0U)) {
        return ENET_ERR_PARAM;
    }

    for (i = 0U; i < ENET_TCP_MAX_CHANNELS; i++) {
        Enet_TcpChannelTypeDef *channel = &h->Channels[i];

        if ((channel->InUse == 0U) || (channel->Pcb == NULL)) {
            continue;
        }

        if (Enet_Tcp_Write(channel, data, len) != ENET_OK) {
            failed++;                       /* 发送窗口满了: 这一路没收到     */
        }
    }

    /* 一个客户端都没有也算成功: 没有人在听并不是错误. */
    return (failed == 0U) ? ENET_OK : ENET_ERR_HW;
}

uint32_t Enet_Tcp_GetActiveChannels(const Enet_HandleTypeDef *h)
{
    uint32_t i;
    uint32_t active = 0U;

    if (h == NULL) {
        return 0U;
    }

    for (i = 0U; i < ENET_TCP_MAX_CHANNELS; i++) {
        if (h->Channels[i].InUse != 0U) {
            active++;
        }
    }

    return active;
}
