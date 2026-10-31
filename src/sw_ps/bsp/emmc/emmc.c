/**
 *******************************************************************************
 * @file    emmc.c
 * @brief   eMMC(SD1)上的文件读写 —— FatFs 薄封装的实现
 *
 * 设计要点(为什么这么写, 见 emmc.h 里更长的说明):
 *   1. 一把**递归**互斥量把 FatFs 串起来 —— 它默认不可重入, 而写数据的任务和回读
 *      命令的 net 任务会同时进来。
 *   2. 所有走 SD 控制器的缓冲都是**静态 + 32 字节对齐**的(gSectorBuf / gWorkBuf),
 *      因为 XPAR_XSDPS_1_IS_CACHE_COHERENT = 0。
 *   3. 回读**不解析字段**, 按行原样交给上层(结构检查只看逗号个数)。
 *   4. 行的切分只有一处实现(Emmc_WalkLines)。**不能**按 512 字节一块一块地找 '\n':
 *      文件偏移不落在扇区边界上时, 一行会横跨两块, 那样会漏行、数错条数 —— 这是
 *      写这个文件时真踩过的坑, 所以扫描和回读共用同一个逐行装配器。
 *******************************************************************************
 */

#define LOG_ENABLE 1    /* 挂载/格式化/打开这类"一次性且失败要命"的动作要留痕 */
#define LOG_TAG "emmc"
#include "service/monitor_module/log/log.h"

#include "bsp/emmc/emmc.h"

#include "ff.h"

#include "osal/osal.h"

#include <string.h>

/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

static FATFS                   gFs;
static FIL                     gFile;
static Osal_MutexHandleTypeDef gLock;
static uint8_t                 gLockReady;
static uint8_t                 gMounted;
static uint8_t                 gFileOpen;

static Emmc_InfoTypeDef        gInfo;

/** "0:/data.csv" 这种路径的最大长度(含结尾 '\0')。 */
#define EMMC_PATH_MAX           16U

/*
 * 当前**实际在用**的盘: 默认是板载 eMMC(SD1), 挂不上就自动切到 SD 卡槽(SD0, 见 Emmc_Mount)。
 * gDataFile = 盘符 + 文件名, 所以三个东西必须一起切 —— 所有 f_open/f_mount 都用它们,
 * 不再直接用 EMMC_VOLUME/EMMC_DATA_FILE 那两个宏(它们是"首选 eMMC"的初值)。
 */
static char    gVolume[8]               = EMMC_VOLUME;
static char    gDataFile[EMMC_PATH_MAX] = EMMC_DATA_FILE;
static uint8_t gDrive                   = EMMC_DRIVE;

/**
 * 走 SD 控制器的缓冲: **静态 + 32 字节对齐**。
 *
 * 为什么不直接用调用者传进来的缓冲: SD 控制器不是缓存一致的(见头文件), 驱动的
 * cache 维护只覆盖它自己拿到的那块内存。用静态对齐缓冲 = 每次 DMA 的目标地址都是
 * 我们知道的、对齐的、而且不在栈上 —— 栈在 Zynq 上同样要走 cache, 而且随时可能被
 * 别的任务压数据。
 */
static u8   gSectorBuf[512] __attribute__((aligned(32)));
static BYTE gWorkBuf[4096]  __attribute__((aligned(32)));

/* 逐行装配用的行缓冲(只在被 gLock 保护的回读/计数路径里用) */
static char gLineBuf[EMMC_LINE_MAX];

/* ========================================================================== */
/*  内部函数(static)                                                           */
/* ========================================================================== */

/** @brief  惰性建锁: 第一次用到时才建, 免得驱动还没初始化就被调进来。 */
static void Emmc_Lock(void)
{
    if (gLockReady == 0U) {
        gLock = Osal_MutexCreateRecursive();
        gLockReady = (gLock != NULL) ? 1U : 0U;
    }

    if (gLockReady != 0U) {
        Osal_MutexLock(gLock);
    }
}

static void Emmc_Unlock(void)
{
    if (gLockReady != 0U) {
        Osal_MutexUnlock(gLock);
    }
}

/** @brief  FatFs 返回码 -> 本模块的错误码。 */
static Emmc_StatusTypeDef Emmc_MapResult(FRESULT Fr)
{
    gInfo.LastError = (uint8_t)Fr;

    switch (Fr) {
        case FR_OK:                  return EMMC_OK;
        case FR_NOT_READY:           return EMMC_ERR_MOUNT;
        case FR_NO_FILESYSTEM:       return EMMC_ERR_MOUNT;
        case FR_NO_FILE:             return EMMC_ERR_NOTFOUND;
        case FR_NO_PATH:             return EMMC_ERR_NOTFOUND;
        case FR_DENIED:              return EMMC_ERR_FULL;
        case FR_TOO_MANY_OPEN_FILES: return EMMC_ERR_FULL;
        case FR_NOT_ENOUGH_CORE:     return EMMC_ERR_FULL;
        case FR_INVALID_OBJECT:      return EMMC_ERR_STATE;
        case FR_INVALID_DRIVE:       return EMMC_ERR_MOUNT;
        case FR_INVALID_PARAMETER:   return EMMC_ERR_PARAM;
        default:                     return EMMC_ERR_IO;
    }
}

/**
 * @brief  这块平台**有没有 SD1** —— 也就是板载 eMMC 能不能被软件看见。
 *
 * 判据是 BSP 的 xparameters.h 里有没有 `XPAR_XSDPS_1_BASEADDR`: 它只在"硬件里使能了
 * SD1(MIO 46..51) **并且平台重新导出**"之后才会出现。没有它的时候, 就算代码硬去
 * f_mount("1:/"), xilffs 的 diskio 也会 `XSdPs_LookupConfig(1)` 返回 NULL, 结果是
 * FR_NOT_READY —— 白试一次还留一行看不懂的错误。所以这里先问平台, 不白试。
 */
static uint8_t Emmc_PlatformHasSd1(void)
{
#if defined(XPAR_XSDPS_1_BASEADDR)
    return 1U;
#else
    return 0U;
#endif
}

/** @brief  FatFs 返回码 -> 一句人看得懂的话(现场靠它定位, 不打哑谜)。 */
static const char *Emmc_FrHint(FRESULT Fr)
{
    switch (Fr) {
        case FR_OK:              return "ok";
        case FR_DISK_ERR:        return "low-level disk error (bus / card)";
        case FR_INT_ERR:         return "internal error";
        case FR_NOT_READY:       return "drive not ready (controller not in platform / no medium)";
        case FR_NO_FILE:         return "file not found";
        case FR_NO_PATH:         return "path not found";
        case FR_INVALID_NAME:    return "invalid name";
        case FR_DENIED:          return "denied (write protected?)";
        case FR_EXIST:           return "already exists";
        case FR_INVALID_OBJECT:  return "invalid object (volume not mounted)";
        case FR_WRITE_PROTECTED: return "write protected";
        case FR_INVALID_DRIVE:   return "invalid drive (not present in this platform)";
        case FR_NOT_ENABLED:     return "volume not enabled";
        case FR_NO_FILESYSTEM:   return "no FAT32 filesystem -> run EMCFMT YES";
        case FR_MKFS_ABORTED:    return "mkfs aborted";
        case FR_TIMEOUT:         return "timeout";
        case FR_NOT_ENOUGH_CORE: return "not enough core (FatFs work area)";
        default:                 return "other FR error";
    }
}

/** @brief  切盘: 盘符 / 数据文件名 / FatFs 逻辑盘号三件套一起换。 */
static void Emmc_UseVolume(const char *Vol, uint8_t Drive)
{
    uint8_t n = 0U;
    uint8_t k;

    while ((Vol[n] != '\0') && (n < (uint8_t)(sizeof(gVolume) - 1U))) {
        gVolume[n] = Vol[n];
        n++;
    }
    gVolume[n] = '\0';

    for (k = 0U; k < n; k++) {
        gDataFile[k] = gVolume[k];
    }
    for (k = 0U; k < 8U; k++) {
        gDataFile[n + k] = "data.csv"[k];       /* 8.3 名字, 不用长文件名 */
    }
    gDataFile[n + 8U] = '\0';

    gDrive = Drive;
}

/** @brief  一行里逗号的个数。 */
static uint32_t Emmc_CountCommas(const char *Line, uint32_t Len)
{
    uint32_t i;
    uint32_t n = 0U;

    for (i = 0U; i < Len; i++) {
        if (Line[i] == ',') {
            n++;
        }
    }

    return n;
}

/**
 * @brief  一行是不是"能用的数据行"。
 *
 * 只做**结构**检查: 10 个字段(t_ms,i1..i5,vbus,speed,x_mm,laser_mv)正好 9 个逗号。表头也满足,
 * 所以调用者还要跳过第一行。半截行(掉电写坏的)字段数不对, 在这里就被丢掉了。
 */
static uint8_t Emmc_IsDataLine(const char *Line, uint32_t Len)
{
    return ((Len > 0U) && (Emmc_CountCommas(Line, Len) == 9U)) ? 1U : 0U;
}

/* -------------------------------------------------------------------------- */
/*  逐行遍历(扫描和回读共用)                                                    */
/* -------------------------------------------------------------------------- */
/** @brief  每读到一个完整行调一次; 返回 0 = 调用者要求停止。 */
typedef int (*Emmc_LineCbTypeDef)(const char *Line, uint32_t Len, void *Ctx);

/**
 * @brief  从文件头顺序读, 每遇到一个**完整行**(不含行尾 \r\n)就回调一次。
 *
 * @param  Fp       : 要遍历的**已打开**文件。为什么不直接用全局 gFile: 挂载之后要
 *                    统计卡上已有的记录数, 那时候得开一个**临时的**只读句柄, 不能
 *                    动 gFile(它可能正开着在录)。
 * @param  Stopped  : 输出: 1 = 是回调要求提前停的(不是读到文件尾)。
 * @param  HalfTail : 输出: 1 = 文件最后一行没有换行收尾(掉电写了一半)。
 *
 * @note   行跨 512 字节读块的情况在这里被正确处理: 字节先攒进 gLineBuf, 遇到 '\n'
 *         才交付。行比 gLineBuf 还长(异常文件)就整行丢掉, 不交付。
 */
static Emmc_StatusTypeDef Emmc_WalkLines(FIL *Fp, Emmc_LineCbTypeDef Cb, void *Ctx,
                                         uint8_t *Stopped, uint8_t *HalfTail)
{
    FRESULT  fr;
    UINT     br;
    uint32_t i;
    uint32_t len = 0U;
    uint8_t  overflow = 0U;
    uint8_t  first = 1U;
    uint8_t  stop = 0U;

    *Stopped  = 0U;
    *HalfTail = 0U;

    fr = f_lseek(Fp, 0);
    if (fr != FR_OK) {
        return Emmc_MapResult(fr);
    }

    for (;;) {
        fr = f_read(Fp, gSectorBuf, sizeof(gSectorBuf), &br);
        if (fr != FR_OK) {
            return Emmc_MapResult(fr);
        }
        if (br == 0U) {
            break;
        }

        for (i = 0U; i < br; i++) {
            const u8 c = gSectorBuf[i];

            if (c == (u8)'\n') {
                if ((len > 0U) && (gLineBuf[len - 1U] == '\r')) {
                    len--;                      /* 去掉行尾的 '\r' */
                }

                if (overflow == 0U) {
                    if (first == 0U) {
                        if (Cb(gLineBuf, len, Ctx) == 0) {
                            stop = 1U;
                            break;
                        }
                    }
                }

                first    = 0U;
                len      = 0U;
                overflow = 0U;
                continue;
            }

            if (len < (uint32_t)sizeof(gLineBuf)) {
                gLineBuf[len++] = (char)c;
            } else {
                overflow = 1U;                  /* 超长行: 整行丢掉 */
            }
        }

        if (stop != 0U) {
            break;
        }
    }

    /* 收尾时还攒着字节 = 最后一行没有换行 -> 半截行, 不回调 */
    if ((stop == 0U) && ((len > 0U) || (overflow != 0U))) {
        *HalfTail = 1U;
    }

    *Stopped = stop;

    return EMMC_OK;
}

/** @brief  数记录条数用的回调上下文。 */
typedef struct {
    uint32_t Records;
} Emmc_ScanCtxTypeDef;

static int Emmc_CountCb(const char *Line, uint32_t Len, void *Ctx)
{
    Emmc_ScanCtxTypeDef *c = (Emmc_ScanCtxTypeDef *)Ctx;

    if (Emmc_IsDataLine(Line, Len) != 0U) {
        c->Records++;
    }

    return 1;
}

/** @brief  写一段文本(调用者已持锁)。 */
static Emmc_StatusTypeDef Emmc_WriteRaw(const char *Text, uint32_t Len, uint32_t *Written)
{
    FRESULT  fr;
    UINT     bw;
    uint32_t done = 0U;
    uint32_t chunk;

    while (done < Len) {
        chunk = Len - done;
        if (chunk > sizeof(gSectorBuf)) {
            chunk = sizeof(gSectorBuf);         /* 一次最多一个扇区, 缓冲就那么大 */
        }

        (void)memcpy(gSectorBuf, Text + done, chunk);

        fr = f_write(&gFile, gSectorBuf, (UINT)chunk, &bw);
        if (fr != FR_OK) {
            return Emmc_MapResult(fr);
        }
        if (bw != (UINT)chunk) {
            return EMMC_ERR_FULL;               /* 卡写满了 */
        }

        done += chunk;
    }

    *Written = done;

    return EMMC_OK;
}

/* ========================================================================== */
/*  挂载 / 卸载 / 格式化                                                        */
/* ========================================================================== */

/**
 * @brief  挂载之后统计一下卡上**已经有多少条记录**(调用者已持锁)。
 *
 * 为什么要有它: gInfo.Records 原来只在"打开文件"时才算(Emmc_OpenData 里那次扫描)。
 * 于是**重启之后第一次敲 EMCSTAT 会报 records=0, 哪怕卡上明明录了一大堆** ——
 * 现场看到这个会以为数据丢了。上板实测时就是这么撞上的(2026/10/09)。
 *
 * 用 FA_READ **单独**打开一遍: 不带 FA_OPEN_ALWAYS, 所以文件不存在时只会返回
 * FR_NO_FILE, **不会凭空建出一个空文件**(EMCMOUNT 只是查询, 不该有副作用)。
 * 而且用的是临时句柄, 不碰 gFile —— 挂载时它一般没开, 但也不能假设。
 */
static void Emmc_RefreshFileStats(void)
{
    FIL     probe;
    FRESULT fr;
    Emmc_ScanCtxTypeDef scan;
    uint8_t stopped = 0U;
    uint8_t halfTail = 0U;

    fr = f_open(&probe, gDataFile, FA_READ);
    if (fr != FR_OK) {
        gInfo.Records = 0U;             /* 还没有这个文件(或者读不了) */
        gInfo.Bytes   = 0U;
        return;
    }

    scan.Records = 0U;
    (void)Emmc_WalkLines(&probe, Emmc_CountCb, &scan, &stopped, &halfTail);

    gInfo.Records = scan.Records;
    gInfo.Bytes   = (uint32_t)f_size(&probe);

    (void)f_close(&probe);
}

Emmc_StatusTypeDef Emmc_Mount(void)
{
    FRESULT fr = FR_NOT_READY;
    DWORD   freeClusters = 0U;
    FATFS  *pfs = NULL;
    Emmc_StatusTypeDef rc = EMMC_ERR_MOUNT;

    /* 候选盘(优先级顺序): 0 = 板载 eMMC(SD1), 1 = SD 卡槽(SD0) */
    static const struct {
        const char *Vol;
        uint8_t     Drive;
        const char *What;
    } cand[2] = {
        { EMMC_VOLUME,     EMMC_DRIVE,     "on-board eMMC (SD1)" },
        { EMMC_VOLUME_ALT, EMMC_DRIVE_ALT, "SD card slot (SD0)"  }
    };

    /* 平台里没有 SD1 时直接跳过 eMMC —— 不白试, 也不留假错误 */
    const uint8_t first = (Emmc_PlatformHasSd1() != 0U) ? 0U : 1U;
    uint8_t idx;
    uint8_t used = first;

    Emmc_Lock();

    for (idx = first; idx < 2U; idx++) {
        Emmc_UseVolume(cand[idx].Vol, cand[idx].Drive);
        used = idx;

        fr = f_mount(&gFs, gVolume, 1);         /* 1 = 立刻挂载 */
        rc = Emmc_MapResult(fr);

        if (rc == EMMC_OK) {
            break;
        }

        /*
         * 只有"这块盘压根儿不存在 / 没介质"(FR_NOT_READY)才换下一块。
         * 别的错(eMMC 在、但没格式化 -> FR_NO_FILESYSTEM; 线/卡坏 -> FR_DISK_ERR)
         * 就**停在这块盘上**: 留着盘符, EMCFMT YES 才能把它格式化出来 —— 悄悄换到
         * 另一块盘会让"为什么数据跑到卡槽去了"变成一个没人能解释的谜。
         */
        if (fr != FR_NOT_READY) {
            break;
        }

        if (idx == 0U) {
            LOG_W("eMMC (%s) not ready: FR=%u (%s) - trying %s\r\n",
                  cand[0].Vol, (unsigned)fr, Emmc_FrHint(fr), cand[1].Vol);
        }
    }

    if (rc == EMMC_OK) {
        gMounted = 1U;

        if (f_getfree(gVolume, &freeClusters, &pfs) == FR_OK) {
            gInfo.FreeClusters  = (uint32_t)freeClusters;
            gInfo.TotalClusters = (uint32_t)(pfs->n_fatent - 2U);
        }

        /* 统计盘上已有的记录: 挂载之后 EMCSTAT 就要能报出真实条数 */
        if (gFileOpen == 0U) {
            Emmc_RefreshFileStats();
        }

        LOG_I("mounted %s (FatFs drive %u, %s): free %u clusters, %u records already there\r\n",
              gVolume, (unsigned)gDrive, cand[used].What,
              (unsigned)gInfo.FreeClusters, (unsigned)gInfo.Records);
    } else {
        gMounted = 0U;
        gInfo.Records = 0U;
        gInfo.Bytes   = 0U;

        LOG_E("mount %s failed: FR=%u (%s)\r\n",
              gVolume, (unsigned)fr, Emmc_FrHint(fr));

        /*
         * 把"该去哪儿改"直接写出来 —— 这一条是这次踩出来的:
         * 硬件里开了 SD1 还不够, 必须重新导出 .xsa + 重新导出平台(BSP), 否则
         * xparameters.h 里没有 XPAR_XSDPS_1_BASEADDR, xilffs 找不到第 1 个控制器。
         */
        if (Emmc_PlatformHasSd1() == 0U) {
            LOG_W("this platform has no SD1 at all (BSP xparameters.h: no XPAR_XSDPS_1_BASEADDR)\r\n");
            LOG_W("the on-board eMMC cannot be reached from software. To enable it: turn on "
                  "PCW_SD1_PERIPHERAL_ENABLE + SD1_IO = MIO 46..51 in the block design, "
                  "write a new .xsa, re-export the platform (and refresh FSBL ps7_init for "
                  "flash boot), then rebuild this app\r\n");
            LOG_W("meanwhile recording goes to the SD card slot (0:/) if a FAT32 card is inserted\r\n");
        }
    }

    gInfo.Mounted = gMounted;

    Emmc_Unlock();

    return rc;
}

void Emmc_Unmount(void)
{
    Emmc_Lock();

    if (gFileOpen != 0U) {
        (void)f_close(&gFile);
        gFileOpen = 0U;
        gInfo.FileOpen = 0U;
    }

    if (gMounted != 0U) {
        (void)f_mount(NULL, gVolume, 0);    /* 0 = 注销 */
        gMounted = 0U;
    }

    gInfo.Mounted = 0U;

    Emmc_Unlock();
}

/**
 * @brief  格式化**当前那块盘**(默认 eMMC, 平台没有 SD1 或挂不上时是 SD 卡槽)。
 *
 * @note   要格式化指定的另一块盘(比如平台两块都在, 想只格卡槽), 用
 *         `Emmc_SelectVolume()` 先切过去 —— service 层的 `EMCFMT YES [0|1]` 就是干这个的。
 */
Emmc_StatusTypeDef Emmc_Format(void)
{
    MKFS_PARM opt;
    FRESULT   fr;
    Emmc_StatusTypeDef rc;

    Emmc_Lock();

    /* 格式化前必须卸载, 否则 f_mkfs 会报 FR_INVALID_OBJECT(卷还挂着) */
    if (gFileOpen != 0U) {
        (void)f_close(&gFile);
        gFileOpen = 0U;
        gInfo.FileOpen = 0U;
    }
    if (gMounted != 0U) {
        (void)f_mount(NULL, gVolume, 0);
        gMounted = 0U;
    }

    opt.fmt     = FM_FAT32;     /* eMMC 一般都 > 2 GB, 直接用 FAT32 */
    opt.n_fat   = 0U;
    opt.align   = 0U;
    opt.n_root  = 0U;
    opt.au_size = 0U;

    fr = f_mkfs(gVolume, &opt, gWorkBuf, sizeof(gWorkBuf));
    rc = Emmc_MapResult(fr);

    LOG_I("mkfs %s -> FR=%u (%s)\r\n", gVolume, (unsigned)fr, Emmc_FrHint(fr));

    Emmc_Unlock();

    if (rc != EMMC_OK) {
        return rc;
    }

    /* 格式化完直接挂上, 免得用户还要再敲一次 EMCMOUNT */
    return Emmc_Mount();
}

/* ========================================================================== */
/*  数据文件                                                                   */
/* ========================================================================== */

Emmc_StatusTypeDef Emmc_OpenData(uint8_t Rewind)
{
    FRESULT  fr;
    uint32_t written = 0U;
    uint8_t  stopped = 0U;
    uint8_t  halfTail = 0U;
    Emmc_ScanCtxTypeDef scan;
    Emmc_StatusTypeDef rc;

    Emmc_Lock();

    if (gMounted == 0U) {
        Emmc_Unlock();
        return EMMC_ERR_MOUNT;
    }

    if (gFileOpen != 0U) {
        (void)f_close(&gFile);
        gFileOpen = 0U;
        gInfo.FileOpen = 0U;
    }

    if (Rewind != 0U) {
        fr = f_open(&gFile, gDataFile, FA_CREATE_ALWAYS | FA_WRITE | FA_READ);
    } else {
        fr = f_open(&gFile, gDataFile, FA_OPEN_ALWAYS | FA_WRITE | FA_READ);
    }

    if (fr != FR_OK) {
        Emmc_Unlock();
        return Emmc_MapResult(fr);
    }

    gFileOpen = 1U;
    gInfo.FileOpen = 1U;
    gInfo.Bytes = (uint32_t)f_size(&gFile);

    if ((Rewind != 0U) || (gInfo.Bytes == 0U)) {
        /* 新文件(或者刚清空): 写表头, 计数归零 */
        rc = Emmc_WriteRaw(EMMC_DATA_HEADER "\r\n",
                           (uint32_t)strlen(EMMC_DATA_HEADER) + 2U, &written);
        if (rc != EMMC_OK) {
            Emmc_Unlock();
            return rc;
        }
        gInfo.Records = 0U;
        gInfo.Bytes   = written;
    } else {
        /* 接着写: 先数清楚里面已经有多少条 */
        scan.Records = 0U;

        rc = Emmc_WalkLines(&gFile, Emmc_CountCb, &scan, &stopped, &halfTail);
        if (rc != EMMC_OK) {
            Emmc_Unlock();
            return rc;
        }

        gInfo.Records = scan.Records;

        if (halfTail != 0U) {
            /* 末尾那条是半截的(掉电所致)。补一个换行把它收尾 —— 它就成了一条字段数
             * 不对的废行, 回读时按"必须 7 个逗号"被跳过, 不会污染曲线。 */
            rc = Emmc_WriteRaw("\r\n", 2U, &written);
            if (rc != EMMC_OK) {
                Emmc_Unlock();
                return rc;
            }
            gInfo.Bytes += written;
            LOG_W("data file had a half-written tail line; terminated it\r\n");
        }
    }

    gInfo.Bytes = (uint32_t)f_size(&gFile);

    Emmc_Unlock();

    return EMMC_OK;
}

Emmc_StatusTypeDef Emmc_CloseData(void)
{
    FRESULT fr;

    Emmc_Lock();

    if (gFileOpen == 0U) {
        Emmc_Unlock();
        return EMMC_ERR_STATE;
    }

    /* 大小要在 f_close **之前**取: f_close 会把 FIL 对象标记成无效, 之后再去读
     * f_size 是在读一个已经作废的对象(现在的 FatFs 实现里它还是对的上的, 但那是
     * 依赖内部细节, 不该赌)。 */
    gInfo.Bytes = (uint32_t)f_size(&gFile);

    fr = f_close(&gFile);                       /* f_close 内部会 f_sync */

    gFileOpen = 0U;
    gInfo.FileOpen = 0U;

    Emmc_Unlock();

    return Emmc_MapResult(fr);
}

uint8_t Emmc_DataIsOpen(void)
{
    return gFileOpen;
}

Emmc_StatusTypeDef Emmc_AppendText(const char *Text, uint32_t Len)
{
    uint32_t written = 0U;
    uint32_t i;
    uint32_t newLines = 0U;
    Emmc_StatusTypeDef rc;

    if ((Text == NULL) || (Len == 0U)) {
        return EMMC_ERR_PARAM;
    }

    Emmc_Lock();

    if ((gMounted == 0U) || (gFileOpen == 0U)) {
        Emmc_Unlock();
        return EMMC_ERR_STATE;
    }

    /*
     * 走静态扇区缓冲, 所以一次只能写 sizeof(gSectorBuf)。调用者(service/emmc)按
     * 一行一次调用, 行最长 EMMC_LINE_MAX(128) —— 远小于 512, 所以一行一定完整落在
     * 同一次 f_write 里。
     */
    if (Len > sizeof(gSectorBuf)) {
        Emmc_Unlock();
        return EMMC_ERR_PARAM;
    }

    rc = Emmc_WriteRaw(Text, Len, &written);
    if (rc != EMMC_OK) {
        Emmc_Unlock();
        return rc;
    }

    for (i = 0U; i < Len; i++) {
        if (Text[i] == '\n') {
            newLines++;
        }
    }

    gInfo.Records += newLines;
    gInfo.Bytes   += written;

    Emmc_Unlock();

    return EMMC_OK;
}

Emmc_StatusTypeDef Emmc_Sync(void)
{
    FRESULT fr;

    Emmc_Lock();

    if ((gMounted == 0U) || (gFileOpen == 0U)) {
        Emmc_Unlock();
        return EMMC_ERR_STATE;
    }

    fr = f_sync(&gFile);

    Emmc_Unlock();

    return Emmc_MapResult(fr);
}

Emmc_StatusTypeDef Emmc_Truncate(void)
{
    /* Emmc_OpenData 自己会加锁, 所以这里不再持锁进来 —— 递归锁虽然扛得住, 但这函数
     * 本身不需要临界区, 少一层更清楚。 */
    return Emmc_OpenData(1U);
}

/* ========================================================================== */
/*  回读                                                                       */
/* ========================================================================== */

/** @brief  回读用的回调上下文。 */
typedef struct {
    uint32_t    Start;      /**< 要跳过的记录条数                           */
    uint32_t    MaxLines;   /**< 这批最多给几行                             */
    uint32_t    Seen;       /**< 已经跳过几条                               */
    uint32_t    Copied;     /**< 已经拷了几行                               */
    uint32_t    Used;       /**< OutBuf 里已经用了多少字节                  */
    uint32_t    OutSize;
    uint32_t    PrefixLen;
    const char *Prefix;     /**< 每行前面的行头(可为 NULL)                  */
    char       *Out;
    uint8_t     More;       /**< 后面还有                                  */
} Emmc_ReadCtxTypeDef;

static int Emmc_ReadCb(const char *Line, uint32_t Len, void *Ctx)
{
    Emmc_ReadCtxTypeDef *c = (Emmc_ReadCtxTypeDef *)Ctx;

    if (Emmc_IsDataLine(Line, Len) == 0U) {
        return 1;                               /* 废行跳过, 继续 */
    }

    if (c->Seen < c->Start) {
        c->Seen++;
        return 1;
    }

    /* 行头 + 行内容 + "\r\n" */
    if ((c->Used + c->PrefixLen + Len + 2U) > c->OutSize) {
        c->More = 1U;
        return 0;                               /* 装不下了, 下一批再来 */
    }

    if (c->PrefixLen > 0U) {
        (void)memcpy(c->Out + c->Used, c->Prefix, c->PrefixLen);
        c->Used += c->PrefixLen;
    }

    if (Len > 0U) {
        (void)memcpy(c->Out + c->Used, Line, Len);
        c->Used += Len;
    }

    c->Out[c->Used++] = '\r';
    c->Out[c->Used++] = '\n';
    c->Copied++;

    if (c->Copied >= c->MaxLines) {
        /* 到了这批的上限。后面还有没有? 这里先保守地报"有": 上位机再拉一次,
         * 如果真没有了下次会拿到 0 行 + More=0, 自然就停了。 */
        c->More = 1U;
        return 0;
    }

    return 1;
}

Emmc_StatusTypeDef Emmc_ReadLines(uint32_t Start, uint32_t MaxLines,
                                  const char *Prefix,
                                  char *OutBuf, uint32_t OutSize,
                                  uint32_t *OutLines, uint32_t *OutLen,
                                  uint8_t *More)
{
    Emmc_ReadCtxTypeDef ctx;
    uint8_t  stopped = 0U;
    uint8_t  halfTail = 0U;
    Emmc_StatusTypeDef rc;

    if ((OutBuf == NULL) || (OutLines == NULL) || (OutLen == NULL) || (More == NULL)) {
        return EMMC_ERR_PARAM;
    }

    *OutLines = 0U;
    *OutLen   = 0U;
    *More     = 0U;

    if (MaxLines == 0U) {
        MaxLines = EMMC_READ_MAX_LINES;
    }

    Emmc_Lock();

    if ((gMounted == 0U) || (gFileOpen == 0U)) {
        Emmc_Unlock();
        return EMMC_ERR_STATE;
    }

    ctx.Start     = Start;
    ctx.MaxLines  = MaxLines;
    ctx.Seen      = 0U;
    ctx.Copied    = 0U;
    ctx.Used      = 0U;
    ctx.OutSize   = OutSize;
    ctx.Out       = OutBuf;
    ctx.More      = 0U;
    ctx.Prefix    = Prefix;
    ctx.PrefixLen = (Prefix != NULL) ? (uint32_t)strlen(Prefix) : 0U;

    rc = Emmc_WalkLines(&gFile, Emmc_ReadCb, &ctx, &stopped, &halfTail);

    /* 遍历把文件指针带到了某处, 追加必须从末尾继续 —— 拉回末尾。 */
    if ((rc == EMMC_OK) && (f_lseek(&gFile, f_size(&gFile)) != FR_OK)) {
        rc = EMMC_ERR_IO;
    }

    Emmc_Unlock();

    if (rc != EMMC_OK) {
        return rc;
    }

    *OutLines = ctx.Copied;
    *OutLen   = ctx.Used;
    *More     = ctx.More;

    return EMMC_OK;
}

/* ========================================================================== */
/*  快照                                                                       */
/* ========================================================================== */

void Emmc_GetInfo(Emmc_InfoTypeDef *Out)
{
    if (Out == NULL) {
        return;
    }

    Emmc_Lock();

    *Out = gInfo;

    Emmc_Unlock();
}

/* ========================================================================== */
/*  当前用哪块盘                                                               */
/* ========================================================================== */

const char *Emmc_GetVolume(void)
{
    return gVolume;
}

uint8_t Emmc_GetDrive(void)
{
    return gDrive;
}

uint8_t Emmc_PlatformHasSd1Vol(void)
{
    return Emmc_PlatformHasSd1();
}

/**
 * @brief  手动指定下一次要用的盘(0 = SD 卡槽, 1 = 板载 eMMC)。
 *
 * @note   只是**换盘符**, 不挂载 —— 调用者随后自己调 Emmc_Mount()。
 *         给 `EMCFMT YES [0|1]` 用: 两块盘都在平台上时, 光靠"挂不上就换一块"是选不
 *         到另一块的(比如 eMMC 好好的, 但你想格式化卡槽里那张卡)。
 */
Emmc_StatusTypeDef Emmc_SelectVolume(uint8_t Drive)
{
    if (Drive > 1U) {
        return EMMC_ERR_PARAM;
    }

    if ((Drive == 1U) && (Emmc_PlatformHasSd1() == 0U)) {
        return EMMC_ERR_MOUNT;      /* 平台上没有 SD1, 切过去也是白搭 */
    }

    Emmc_Lock();

    if (Drive == 1U) {
        Emmc_UseVolume(EMMC_VOLUME, EMMC_DRIVE);
    } else {
        Emmc_UseVolume(EMMC_VOLUME_ALT, EMMC_DRIVE_ALT);
    }

    gMounted = 0U;                  /* 换了盘, 之前的挂载状态就不作数了 */
    gInfo.Mounted = 0U;

    Emmc_Unlock();

    return EMMC_OK;
}
