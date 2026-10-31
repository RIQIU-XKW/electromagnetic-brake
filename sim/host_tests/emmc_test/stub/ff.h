/**
 * 宿主机测试用: FatFs(ff.h)的替身。
 *
 * bsp/emmc/emmc.c 里 include 的是 "ff.h", 测试时 -I 把本目录排在
 * third_party/xilffs/include 前面, 于是编的是这份桩, 真实文件系统换成内存里的一段
 * 字节(host_ff.c)。
 *
 * 只声明 emmc.c 真正用到的东西。**故意不实现**没用到的那一大票 FatFs 接口 ——
 * 少一个声明, emmc.c 里多调一个不存在的函数就会当场编译失败, 比链接时才发现好。
 */
#ifndef FF_H
#define FF_H

#include <stdint.h>

/* ---- 基本类型(和 FatFs 保持一致) ---- */
typedef unsigned char  BYTE;
typedef unsigned int   UINT;
/*
 * DWORD / FSIZE_t: 目标平台上 FatFs 用的是 `unsigned long`(ARM 32 位下就是 32 位)。
 * 宿主机是 Windows, `unsigned long` 在 LLP64 下也是 32 位, 但类型名不同会让
 * uint32_t 和它比较时冒出一堆 -Wsign-compare/-Wtype-limits —— 纯粹是宿主机编译器的
 * 噪音, 和被测逻辑无关。所以桩里直接定成 uint32_t: 宽度一样, 语义一样, 少一堆噪音。
 */
typedef uint32_t       DWORD;
typedef char           TCHAR;
typedef uint32_t       FSIZE_t;

/*
 * Xilinx 的短类型名。真机上它们是 xil_types.h 给的(真实 ff.h -> xsdps.h -> xil_types.h
 * 那条链), 桩里没有那条链, 所以自己补 —— bsp/emmc/emmc.c 用了 u8。
 */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  s32;

/* ---- 返回码(只列 emmc.c 会判的) ---- */
typedef enum {
    FR_OK = 0,
    FR_DISK_ERR,
    FR_INT_ERR,
    FR_NOT_READY,
    FR_NO_FILE,
    FR_NO_PATH,
    FR_INVALID_NAME,
    FR_DENIED,
    FR_EXIST,
    FR_INVALID_OBJECT,
    FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE,
    FR_NOT_ENABLED,
    FR_NO_FILESYSTEM,
    FR_MKFS_ABORTED,
    FR_TIMEOUT,
    FR_LOCKED,
    FR_NOT_ENOUGH_CORE,
    FR_TOO_MANY_OPEN_FILES,
    FR_INVALID_PARAMETER
} FRESULT;

/* ---- 打开模式 ---- */
#define FA_READ             0x01U
#define FA_WRITE            0x02U
#define FA_OPEN_EXISTING    0x00U
#define FA_CREATE_NEW       0x04U
#define FA_CREATE_ALWAYS    0x08U
#define FA_OPEN_ALWAYS      0x10U

/* ---- f_mkfs 的参数 ---- */
#define FM_FAT      0x01U
#define FM_FAT32    0x02U
#define FM_EXFAT    0x04U
#define FM_ANY      0x07U

typedef struct {
    BYTE  fmt;
    BYTE  n_fat;
    UINT  align;
    UINT  n_root;
    DWORD au_size;
} MKFS_PARM;

/* ---- 对象(桩里只放测试需要的最小内容; n_fatent 给 Emmc_Mount 读) ---- */
typedef struct {
    DWORD n_fatent;             /* = 簇数 + 2(和真实 FatFs 的语义一致) */
    DWORD csize;
} FATFS;

/** 文件对象。桩把"打开的文件"直接映射到内存文件的一个游标。 */
typedef struct {
    FSIZE_t fptr;               /* 当前读写位置                        */
    BYTE    isOpen;
} FIL;

/* ---- emmc.c 用到的接口 ---- */
FRESULT f_mount(FATFS *fs, const TCHAR *path, BYTE opt);
FRESULT f_mkfs(const TCHAR *path, const MKFS_PARM *opt, void *work, UINT len);
FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT f_close(FIL *fp);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_lseek(FIL *fp, FSIZE_t ofs);
FSIZE_t f_size(FIL *fp);
FRESULT f_sync(FIL *fp);
FRESULT f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs);

#endif /* FF_H */
