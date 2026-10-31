/**
 * 宿主机测试用: 内存文件系统 —— 实现 stub/ff.h 里那几个 FatFs 接口。
 *
 * 语义尽量贴近真实的 FatFs, 特别是这几条(bsp/emmc.c 的逻辑就是围着它们写的):
 *   - f_write 从当前游标往后写, 可以写到文件末尾之外(自动变长); 中间不会补洞, 因为
 *     emmc.c 只做"打开→(清空)→写到末尾"这件事;
 *   - f_open(FA_CREATE_ALWAYS) 清空; FA_OPEN_ALWAYS 保留;
 *   - f_lseek 可以定位到末尾之外? 真实 FatFs 允许(会补 0), 但 emmc.c 只 lseek 到
 *     0 和 f_size() 两处, 所以桩里越界定位直接算错就好 —— 越界反而能暴露 bug;
 *   - f_read 短读(f_read 返回的 br 可能小于请求), 并且**按 512 字节的块**返回:
 *     真实 SD 就是按块读的, bsp/emmc.c 的逐行装配必须扛得住"一次只读到几百字节、
 *     一行横跨两次读"。桩按块短读就是在压这条路径。
 *
 * 另外提供测试用的检查接口(HostFs_*)。
 */
#include "ff.h"

#include <stdio.h>
#include <string.h>

#define HOST_FS_CAP   (512U * 1024U)    /* 512 KB 的虚拟卡, 够测了 */

static BYTE  sData[HOST_FS_CAP];
static DWORD sSize;                     /* 当前文件字节数(只有一个文件) */
static FIL   sFile;
static FATFS sFatFs;

/* 统计: 看看 emmc.c 到底调了几次底层 */
static unsigned sWrites;
static unsigned sReads;
static unsigned sSyncs;
static FRESULT  sForceWriteError = FR_OK;   /* 非 OK 时 f_write 直接返回它 */
static FRESULT  sForceReadError  = FR_OK;
static unsigned sForceAfterN     = 0U;      /* 前 N 次成功, 之后返回上面的错 */
static unsigned sWriteCalls;

static unsigned sShortReadChunk = 512U;     /* f_read 一次最多给多少字节 */

/* ========================================================================== */

FRESULT f_mount(FATFS *fs, const TCHAR *path, BYTE opt)
{
    (void)path;
    (void)opt;

    if (fs != NULL) {
        sFatFs.n_fatent = (HOST_FS_CAP / 512U) + 2U;
        sFatFs.csize    = 1U;
        *fs = sFatFs;
    }

    return FR_OK;
}

FRESULT f_mkfs(const TCHAR *path, const MKFS_PARM *opt, void *work, UINT len)
{
    (void)path;
    (void)opt;
    (void)work;
    (void)len;

    sSize = 0U;
    (void)memset(sData, 0, sizeof(sData));

    return FR_OK;
}

FRESULT f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs)
{
    (void)path;

    if (nclst != NULL) {
        *nclst = 1000U;
    }
    if (fatfs != NULL) {
        *fatfs = &sFatFs;
    }

    return FR_OK;
}

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode)
{
    if ((fp == NULL) || (path == NULL)) {
        return FR_INVALID_PARAMETER;
    }

    if ((mode & FA_CREATE_ALWAYS) != 0U) {
        sSize = 0U;                     /* 清空 */
    } else if ((mode & FA_OPEN_ALWAYS) == 0U) {
        if (sSize == 0U) {
            return FR_NO_FILE;          /* 只读打开且文件不存在 */
        }
    }

    sFile.fptr   = (mode & FA_CREATE_ALWAYS) ? 0U : sSize;
    sFile.isOpen = 1U;
    *fp = sFile;

    return FR_OK;
}

FRESULT f_close(FIL *fp)
{
    if ((fp == NULL) || (fp->isOpen == 0U)) {
        return FR_INVALID_OBJECT;
    }

    sSize = (sFile.fptr > sSize) ? sFile.fptr : sSize;   /* 把游标之前的都算进文件 */
    fp->isOpen = 0U;

    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
    sWrites++;
    sWriteCalls++;

    if ((sForceWriteError != FR_OK) && (sWriteCalls > sForceAfterN)) {
        if (bw != NULL) {
            *bw = 0U;
        }
        return sForceWriteError;
    }

    if ((fp == NULL) || (buff == NULL) || (fp->isOpen == 0U)) {
        return FR_INVALID_OBJECT;
    }

    if ((fp->fptr + btw) > HOST_FS_CAP) {
        if (bw != NULL) {
            *bw = 0U;
        }
        return FR_DENIED;               /* 卡满 */
    }

    (void)memcpy(&sData[fp->fptr], buff, btw);
    fp->fptr += btw;

    if (fp->fptr > sSize) {
        sSize = (DWORD)fp->fptr;
    }

    if (bw != NULL) {
        *bw = btw;
    }

    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
    UINT want;

    sReads++;

    if ((sForceReadError != FR_OK) && (sReads > sForceAfterN)) {
        if (br != NULL) {
            *br = 0U;
        }
        return sForceReadError;
    }

    if ((fp == NULL) || (buff == NULL) || (fp->isOpen == 0U)) {
        return FR_INVALID_OBJECT;
    }

    if (fp->fptr >= sSize) {
        if (br != NULL) {
            *br = 0U;
        }
        return FR_OK;                   /* 到文件尾 */
    }

    want = (UINT)(sSize - fp->fptr);
    if (want > btr) {
        want = btr;
    }

    /* 按块短读: 真实 SD 一次一个块, 一行横跨两次读的情况必须能处理 */
    if (want > sShortReadChunk) {
        want = sShortReadChunk;
    }

    (void)memcpy(buff, &sData[fp->fptr], want);
    fp->fptr += want;

    if (br != NULL) {
        *br = want;
    }

    return FR_OK;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs)
{
    if ((fp == NULL) || (fp->isOpen == 0U)) {
        return FR_INVALID_OBJECT;
    }

    fp->fptr = ofs;

    return FR_OK;
}

FSIZE_t f_size(FIL *fp)
{
    (void)fp;

    return sSize;
}

FRESULT f_sync(FIL *fp)
{
    (void)fp;

    sSyncs++;

    return FR_OK;
}

/* ========================================================================== */
/*  测试用的检查接口                                                           */
/* ========================================================================== */

/** @brief  当前文件内容(不是以 '\0' 结尾的字符串, 要用 HostFs_Size 配合)。 */
const char *HostFs_Data(void) { return (const char *)sData; }

DWORD HostFs_Size(void) { return sSize; }

unsigned HostFs_Writes(void) { return sWrites; }
unsigned HostFs_Reads(void)  { return sReads; }
unsigned HostFs_Syncs(void)  { return sSyncs; }

void HostFs_Reset(void)
{
    (void)memset(sData, 0, sizeof(sData));
    sSize = 0U;
    (void)memset(&sFile, 0, sizeof(sFile));

    sWrites = 0U;
    sReads  = 0U;
    sSyncs  = 0U;
    sWriteCalls = 0U;
    sForceWriteError = FR_OK;
    sForceReadError  = FR_OK;
    sForceAfterN     = 0U;
    sShortReadChunk  = 512U;
}

/** @brief  让接下来的 f_write 失败(模拟卡被拔掉/写坏)。 */
void HostFs_FailWritesAfter(unsigned n, FRESULT fr)
{
    sForceAfterN     = n;
    sForceWriteError = fr;
    sWriteCalls      = 0U;
}

/** @brief  让 f_read 一次最多只给 n 字节(压"一行跨两次读")。 */
void HostFs_SetShortReadChunk(unsigned n) { sShortReadChunk = (n == 0U) ? 1U : n; }

/** @brief  直接往"卡"上写一段字节(测试用来伪造掉电写了一半的尾巴)。 */
void HostFs_Poke(const char *Text, DWORD At)
{
    const DWORD len = (DWORD)strlen(Text);

    if ((At + len) > HOST_FS_CAP) {
        return;
    }

    (void)memcpy(&sData[At], Text, len);
    if ((At + len) > sSize) {
        sSize = At + len;
    }
}
