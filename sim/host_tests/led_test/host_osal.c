/**
 * 宿主机测试用: OSAL 的 pthread 替身 + net 回复桩。
 *
 * 为什么要这么干: service/monitor_module/led/service_led.c 的真实结构是 "收到的命令进队列,
 * 由 led 任务出队执行"。为了把这条路径整个测到(而不是只测驱动), 这里用 pthread
 * 实现一个最小可用的 OSAL:
 *
 *   Osal_QueueCreate/Send/Receive -> 有界环形队列 + 条件变量
 *   Osal_TaskCreate               -> 真起一条 pthread 跑任务函数
 *
 * 于是 Svc_Led_Init() + Osal_TaskCreate(led) 之后, led 任务就是真的在跑;
 * 测试发一条命令 -> 队列 -> 任务出队 -> 写寄存器, 和板子上是同一条路。
 * (用 pthread 而不是 "同步执行任务函数", 是因为 Svc_Led_Task 是 for(;;) 死循环,
 *  没法同步跑完返回。)
 *
 * 测试的同步方式不是 sleep 猜时间, 而是等 Svc_Led_GetSnapshot().Commands 涨到
 * 期望值 —— 那个计数是在寄存器写完**之后**才加的, 所以看到它就说明写完了。
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osal/osal.h"
#include "service/inter_module/service_net.h"

/* ========================================================================== */
/*  队列                                                                       */
/* ========================================================================== */

typedef struct {
    uint32_t        Length;
    uint32_t        ItemSize;
    uint8_t        *Buf;
    uint32_t        Head;               /* 出队位置                          */
    uint32_t        Count;
    pthread_mutex_t Mtx;
    pthread_cond_t  NotEmpty;
} HostQueue;

Osal_QueueHandleTypeDef Osal_QueueCreate(uint32_t Length, uint32_t ItemSize)
{
    HostQueue *q;

    if ((Length == 0U) || (ItemSize == 0U)) {
        return NULL;
    }

    q = (HostQueue *)calloc(1U, sizeof(HostQueue));
    if (q == NULL) {
        return NULL;
    }

    q->Buf = (uint8_t *)calloc(Length, ItemSize);
    if (q->Buf == NULL) {
        free(q);
        return NULL;
    }

    q->Length   = Length;
    q->ItemSize = ItemSize;
    (void)pthread_mutex_init(&q->Mtx, NULL);
    (void)pthread_cond_init(&q->NotEmpty, NULL);

    return (Osal_QueueHandleTypeDef)q;
}

Osal_StatusTypeDef Osal_QueueSend(Osal_QueueHandleTypeDef Q, const void *Item, uint32_t WaitMs)
{
    HostQueue *q = (HostQueue *)Q;
    uint32_t   slot;

    (void)WaitMs;                       /* 测试里队列永远够深, 不做超时逻辑 */

    if ((q == NULL) || (Item == NULL)) {
        return OSAL_ERR_PARAM;
    }

    (void)pthread_mutex_lock(&q->Mtx);

    if (q->Count >= q->Length) {
        (void)pthread_mutex_unlock(&q->Mtx);
        return OSAL_ERR_FULL;
    }

    slot = (q->Head + q->Count) % q->Length;
    (void)memcpy(q->Buf + (slot * q->ItemSize), Item, q->ItemSize);
    q->Count++;

    (void)pthread_cond_signal(&q->NotEmpty);
    (void)pthread_mutex_unlock(&q->Mtx);

    return OSAL_OK;
}

Osal_StatusTypeDef Osal_QueueReceive(Osal_QueueHandleTypeDef Q, void *Item, uint32_t WaitMs)
{
    HostQueue *q = (HostQueue *)Q;

    if ((q == NULL) || (Item == NULL)) {
        return OSAL_ERR_PARAM;
    }

    (void)pthread_mutex_lock(&q->Mtx);

    while (q->Count == 0U) {
        if (WaitMs == OSAL_WAIT_NOWAIT) {
            (void)pthread_mutex_unlock(&q->Mtx);
            return OSAL_ERR_TIMEOUT;
        }
        (void)pthread_cond_wait(&q->NotEmpty, &q->Mtx);
    }

    (void)memcpy(Item, q->Buf + (q->Head * q->ItemSize), q->ItemSize);
    q->Head = (q->Head + 1U) % q->Length;
    q->Count--;

    (void)pthread_mutex_unlock(&q->Mtx);

    return OSAL_OK;
}

/* ========================================================================== */
/*  任务                                                                       */
/* ========================================================================== */

static void *HostTaskTrampoline(void *Arg)
{
    Osal_TaskFuncTypeDef func = NULL;
    void                *arg  = NULL;
    void               **pair = (void **)Arg;

    func = (Osal_TaskFuncTypeDef)pair[0];
    arg  = pair[1];

    func(arg);

    return NULL;
}

Osal_StatusTypeDef Osal_TaskCreate(const char *Name, Osal_TaskFuncTypeDef Func, void *Arg,
                                   uint32_t StackWords, uint32_t Priority,
                                   Osal_TaskHandleTypeDef *Out)
{
    pthread_t  tid;
    void     **pair;

    (void)Name;
    (void)StackWords;
    (void)Priority;

    if (Func == NULL) {
        return OSAL_ERR_PARAM;
    }

    /* 参数要活到线程起来, 所以用堆上的一对小指针(本测试只建一条任务, 不回收) */
    pair    = (void **)calloc(2U, sizeof(void *));
    pair[0] = (void *)Func;
    pair[1] = Arg;

    if (pthread_create(&tid, NULL, HostTaskTrampoline, pair) != 0) {
        free(pair);
        return OSAL_ERR_NOMEM;
    }

    (void)pthread_detach(tid);

    if (Out != NULL) {
        *Out = (Osal_TaskHandleTypeDef)(uintptr_t)tid;
    }

    return OSAL_OK;
}

/* ========================================================================== */
/*  其它桩                                                                     */
/* ========================================================================== */

/*
 * Svc_Mon_Task 的**空实现**。
 *
 * app/monitor_app.c 里有两条东西: ① LED 命令处理(本测试要测的), ② Monitor_App_Start()
 * —— 它建 led 和 mon 两条任务。测试只编了 LED 那一侧的代码(service_monitor.c 没编:
 * 它要拉进 net/pwm 的快照与告警轮询, 连带整个 lwIP), 于是 Monitor_App_Start 里对
 * Svc_Mon_Task 的引用在链接时找不到符号。
 *
 * 给个空函数就够了: 本测试只调 Monitor_App_Command(), 从来不调 Monitor_App_Start(),
 * 那个符号一次都不会被执行到 —— 它只是为了让"生产代码里那条引用"能链上。
 */
void Svc_Mon_Task(void *Arg)
{
    (void)Arg;
}

/* ========================================================================== */
/*  net 回复桩: 把回复文本抓到一个全局缓冲里给测试断言                          */
/* ========================================================================== */

#define HOST_REPLY_MAX  512U

static char     sReply[HOST_REPLY_MAX];
static uint16_t sReplyLen;
static uint32_t sReplyCount;

Svc_StatusTypeDef Svc_Net_Reply(Svc_Net_ReplyTypeDef *R, const char *Text, uint16_t Len)
{
    (void)R;

    if (Text == NULL) {
        return SVC_ERR_PARAM;
    }

    /* 这里**不替组件纠正长度**: 传进来多少就抄多少。组件要是把 Len 算错了(本工程
     * 真出过这种事: service_ads.c 里三处手数的长度写错, 少一个字节会把 \r\n 切掉),
     * 抄出来的字符串就会出现 "结尾不是 \r\n" 或 "带出一个野字符" 的现象 —— 测试用
     * strlen(HostNet_ReplyText()) 和 HostNet_ReplyLen() 比一下就能抓到。 */
    sReplyLen = (Len < (HOST_REPLY_MAX - 1U)) ? Len : (HOST_REPLY_MAX - 1U);
    (void)memcpy(sReply, Text, sReplyLen);
    sReply[sReplyLen] = '\0';
    sReplyCount++;

    return SVC_OK;
}

const char *HostNet_ReplyText(void) { return sReply; }
uint16_t    HostNet_ReplyLen(void)  { return sReplyLen; }
uint32_t    HostNet_ReplyCount(void) { return sReplyCount; }

/** 清空抓到的回复 —— **计数也要清零**, 否则测试没法用 "正好回了一句" 来断言。 */
void HostNet_ReplyReset(void)
{
    sReply[0] = '\0';
    sReplyLen = 0U;
    sReplyCount = 0U;
}
