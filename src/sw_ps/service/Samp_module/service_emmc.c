/**
 *******************************************************************************
 * @file    service_emmc.c
 * @brief   把 ADS 样本录到 eMMC: 拼 CSV 行 -> 攒满 512 B 写盘 -> 每秒 sync
 *
 * 加锁顺序只有一种(所以不会死锁): gStageLock -> bsp/emmc 内部的 FatFs 锁。
 * 本组件没有命令表: 卡挂上且在采样就一直录。
 *******************************************************************************
 */

#define LOG_ENABLE 1
#define LOG_TAG "emmc"
#include "service/monitor_module/log/log.h"

#include "service/Samp_module/service_emmc.h"

#include "service/inter_module/net_text.h"
#include "osal/osal.h"

#include <string.h>

/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

static Osal_QueueHandleTypeDef gQ;
static Osal_MutexHandleTypeDef gStageLock;

/** 攒行用的暂存缓冲(只有 emmc 任务写)。 */
static char     gStage[SVC_EMMC_STAGE_BYTES];
static uint32_t gStageLen;

static volatile uint8_t  gRecording = 1U;   /**< 1 = 录(卡挂上且采样在跑就录)  */
static volatile uint8_t  gStarted;
static volatile uint32_t gT0;               /**< 本次录制起点(uptime ms), 0 = 未定 */

static volatile uint32_t gSamples;
static volatile uint32_t gDropped;
static volatile uint32_t gErrors;
static volatile uint32_t gSyncs;

/* ========================================================================== */
/*  内部函数(static)                                                           */
/* ========================================================================== */

/** @brief  追加一个有符号十进制整数。 @return 新的写入位置。 */
static uint16_t Svc_Emmc_AppendInt(char *Dst, uint16_t Pos, uint16_t Size, int32_t Value)
{
    char     tmp[12];
    uint8_t  n = 0U;
    uint32_t mag;
    uint8_t  neg = 0U;

    if (Value < 0) {
        neg = 1U;
        /* 取绝对值先转 int64: INT32_MIN 直接取负会溢出 */
        mag = (uint32_t)(-(int64_t)Value);
    } else {
        mag = (uint32_t)Value;
    }

    do {
        tmp[n] = (char)('0' + (char)(mag % 10U));
        n++;
        mag /= 10U;
    } while ((mag != 0U) && (n < (uint8_t)sizeof(tmp)));

    if (neg != 0U) {
        if (Pos < (uint16_t)(Size - 1U)) {
            Dst[Pos] = '-';
            Pos++;
        }
    }

    while (n > 0U) {
        n--;
        if (Pos < (uint16_t)(Size - 1U)) {
            Dst[Pos] = tmp[n];
            Pos++;
        }
    }

    Dst[Pos] = '\0';

    return Pos;
}

/** @brief  追加一个千分之一单位的定点数(固定 3 位小数), 和实时 DATA 行同量纲。 */
static uint16_t Svc_Emmc_AppendMilli(char *Dst, uint16_t Pos, uint16_t Size, int32_t Value)
{
    uint32_t mag;
    uint32_t fraction;

    if (Value < 0) {
        if (Pos < (uint16_t)(Size - 1U)) {
            Dst[Pos] = '-';
            Pos++;
        }
        mag = (uint32_t)(-(int64_t)Value);
    } else {
        mag = (uint32_t)Value;
    }

    fraction = mag % 1000U;

    Pos = Svc_Emmc_AppendInt(Dst, Pos, Size, (int32_t)(mag / 1000U));

    if (Pos < (uint16_t)(Size - 1U)) {
        Dst[Pos] = '.';
        Pos++;
    }

    if (fraction < 100U) {
        if (Pos < (uint16_t)(Size - 1U)) {
            Dst[Pos] = '0';
            Pos++;
        }
    }
    if (fraction < 10U) {
        if (Pos < (uint16_t)(Size - 1U)) {
            Dst[Pos] = '0';
            Pos++;
        }
    }

    return Svc_Emmc_AppendInt(Dst, Pos, Size, (int32_t)fraction);
}

/**
 * @brief  一条样本 -> 一行 CSV(带 "\r\n")。
 * @return 行长(0 = 字段数不对, 那行会被回读静默丢掉, 所以当场自检)。
 */
static uint16_t Svc_Emmc_BuildLine(char *Dst, uint16_t Size, uint32_t TimeMs,
                                   const Svc_Ads_SampleTypeDef *s)
{
    uint16_t pos = 0U;
    uint8_t  i;

    pos = Svc_Emmc_AppendInt(Dst, pos, Size, (int32_t)TimeMs);

    /* 线圈 1..5: 电流(A, 3 位小数) */
    for (i = 0U; i < (uint8_t)SVC_ADS_COILS; i++) {
        Dst[pos] = ',';
        pos++;
        pos = Svc_Emmc_AppendMilli(Dst, pos, Size, s->CoilMilliAmps[i]);
    }

    /* 后面几列: vbus = 母线电压(V, 已按 11:1 还原), speed = 激光测速(mm/s),
     * x_mm = 激光位移(mm, 2026/10 新增), laser_mv = 激光引脚电压(mV, 2026/11 新增, 排障用);
     * 列顺序和 DATA 行相反: DATA 行是 speed 在前、vbus 在后, CSV 是 vbus 先 */
    Dst[pos] = ',';
    pos++;
    pos = Svc_Emmc_AppendMilli(Dst, pos, Size, s->VbusMilliVolts);

    Dst[pos] = ',';
    pos++;
    pos = Svc_Emmc_AppendMilli(Dst, pos, Size, s->SpeedMilliMmS);

    Dst[pos] = ',';
    pos++;
    pos = Svc_Emmc_AppendMilli(Dst, pos, Size, s->LaserMilliMm);

    Dst[pos] = ',';
    pos++;
    pos = Svc_Emmc_AppendMilli(Dst, pos, Size, s->LaserMilliVolts);

    Dst[pos] = '\r';
    pos++;
    Dst[pos] = '\n';
    pos++;
    Dst[pos] = '\0';

    /* 必须正好 9 个逗号 = 10 个字段: bsp/emmc 回读时按这个判断一行是否有效 */
    {
        uint16_t k;
        uint8_t  commas = 0U;

        for (k = 0U; k < pos; k++) {
            if (Dst[k] == ',') {
                commas++;
            }
        }

        if (commas != 9U) {
            return 0U;
        }
    }

    return pos;
}

/** @brief  把暂存缓冲写盘并清空(调用者必须已持 gStageLock)。 */
static Svc_StatusTypeDef Svc_Emmc_FlushLocked(void)
{
    Emmc_StatusTypeDef rc;

    if (gStageLen == 0U) {
        return SVC_OK;
    }

    if (Emmc_DataIsOpen() == 0U) {
        gStageLen = 0U;                 /* 没打开就别攒了 */
        return SVC_ERR_STATE;
    }

    rc = Emmc_AppendText(gStage, gStageLen);

    if (rc != EMMC_OK) {
        gErrors++;
        /* 写失败不清空(卡可能只是忙, 下一拍再试); 只有"文件没开"才丢弃 */
        if (rc == EMMC_ERR_STATE) {
            gStageLen = 0U;
        }
        return SVC_ERR_HW;
    }

    gStageLen = 0U;

    return SVC_OK;
}

/** @brief  把暂存缓冲写盘(自己拿锁)。 */
static void Svc_Emmc_Flush(void)
{
    Osal_MutexLock(gStageLock);
    (void)Svc_Emmc_FlushLocked();
    Osal_MutexUnlock(gStageLock);
}

/** @brief  收到一条样本: 拼行 + 攒进暂存, 满了就写盘(在 emmc 任务里跑)。 */
static void Svc_Emmc_Store(const Svc_Ads_SampleTypeDef *s)
{
    char     line[EMMC_LINE_MAX];
    uint16_t len;
    uint32_t tRel;

    gSamples++;

    if ((gRecording == 0U) || (Emmc_DataIsOpen() == 0U)) {
        gDropped++;
        return;
    }

    if (gT0 == 0U) {
        gT0 = s->UptimeMs;              /* 第一拍定起点 */
    }
    tRel = s->UptimeMs - gT0;

    len = Svc_Emmc_BuildLine(line, (uint16_t)sizeof(line), tRel, s);
    if (len == 0U) {
        gErrors++;
        return;
    }

    Osal_MutexLock(gStageLock);

    if ((gStageLen + len) > (uint32_t)sizeof(gStage)) {
        (void)Svc_Emmc_FlushLocked();
    }

    if ((gStageLen + len) <= (uint32_t)sizeof(gStage)) {
        (void)memcpy(&gStage[gStageLen], line, len);
        gStageLen += len;
    } else {
        gErrors++;                      /* 刷完仍装不下(单行超缓冲) */
    }

    if (gStageLen >= (uint32_t)sizeof(gStage)) {
        (void)Svc_Emmc_FlushLocked();
    }

    Osal_MutexUnlock(gStageLock);
}

/* ========================================================================== */
/*  任务函数                                                                     */
/* ========================================================================== */

void Svc_Emmc_Task(void *Arg)            /* 创建由应用层负责, 见 service_emmc.h */
{
    Svc_Ads_SampleTypeDef s;
    uint32_t lastSync;

    (void)Arg;

    lastSync = Osal_UptimeMs();

    LOG_I("task started\r\n");

    for (;;) {
        if (Osal_QueueReceive(gQ, &s, SVC_EMMC_POLL_MS) == OSAL_OK) {
            Svc_Emmc_Store(&s);         /* 只攒, 满了才写盘 */
        } else {
            Svc_Emmc_Flush();           /* 队列超时: 把攒着的那点写下去 */
        }

        if ((Osal_UptimeMs() - lastSync) >= SVC_EMMC_SYNC_MS) {
            lastSync = Osal_UptimeMs();

            Svc_Emmc_Flush();           /* 先落盘再 sync */

            if (Emmc_DataIsOpen() != 0U) {
                if (Emmc_Sync() == EMMC_OK) {
                    gSyncs++;
                } else {
                    gErrors++;
                }
            }
        }
    }
}

/** @brief  错误码 -> 可读文本(日志用)。 */
static const char *Svc_Emmc_ErrName(Emmc_StatusTypeDef Rc)
{
    switch (Rc) {
        case EMMC_OK:            return "ok";
        case EMMC_ERR_PARAM:     return "bad arg";
        case EMMC_ERR_STATE:     return "not mounted / file closed";
        case EMMC_ERR_MOUNT:     return "mount failed (no card / not FAT32 / SD1 off)";
        case EMMC_ERR_IO:        return "io error (card removed?)";
        case EMMC_ERR_FULL:      return "disk full";
        case EMMC_ERR_NOTFOUND:  return "file not found";
        case EMMC_ERR_BUFFER:    return "buffer too small";
        default:                 return "?";
    }
}

/* ========================================================================== */
/*  对外接口                                                                */
/* ========================================================================== */

Svc_StatusTypeDef Svc_Emmc_Init(void)
{
    Emmc_StatusTypeDef rc;

    gStageLock = Osal_MutexCreateRecursive();
    if (gStageLock == NULL) {
        LOG_E("staging lock creation failed\r\n");
        return SVC_ERR_HW;
    }

    gQ = Osal_QueueCreate(SVC_EMMC_Q_LEN, sizeof(Svc_Ads_SampleTypeDef));
    if (gQ == NULL) {
        LOG_E("sample queue creation failed\r\n");
        return SVC_ERR_HW;
    }

    rc = Emmc_Mount();
    if (rc != EMMC_OK) {
        /* 挂载失败不返回错误(没卡时其它功能照常跑); 挂载只在这里做一次, 之后插卡要重启 */
        LOG_W("not mounted (%s) - recording disabled\r\n",
              Svc_Emmc_ErrName(rc));
    }

    gStarted = 1U;

    return SVC_OK;
}

uint8_t Svc_Emmc_IsReady(void)
{
    return gStarted;
}

void Svc_Emmc_OnSample(const Svc_Ads_SampleTypeDef *Sample)
{
    /* 跑在 ADS 任务里: 不录就立刻返回, 入队非阻塞, 满了丢并计数 */
    if ((Sample == NULL) || (gRecording == 0U) || (gQ == NULL)) {
        return;
    }

    if (Osal_QueueSend(gQ, Sample, OSAL_WAIT_NOWAIT) != OSAL_OK) {
        gDropped++;
    }
}

Svc_StatusTypeDef Svc_Emmc_GetSnapshot(Svc_Emmc_SnapshotTypeDef *Out)
{
    if (Out == NULL) {
        return SVC_ERR_PARAM;
    }

    Emmc_GetInfo(&Out->Disk);

    Out->Recording = gRecording;
    Out->Started   = gStarted;
    Out->Samples   = gSamples;
    Out->Written   = Out->Disk.Records;     /* 记录条数以文件实际内容为准 */
    Out->Dropped   = gDropped;
    Out->Errors    = gErrors;
    Out->Syncs     = gSyncs;
    Out->T0Ms      = gT0;

    return SVC_OK;
}
