/**
 *******************************************************************************
 * @file    emmc.h
 * @brief   eMMC(SD1)上的文件读写 —— FatFs 之上的薄封装
 *
 * 只做"挂载、打开、追加、回读、截断"这些文件动作, 不认识业务含义(那在 service 层)。
 *
 * 硬件: eMMC 接 PS 的 SD1(XSDPS_1 @ 0xE0101000), FatFs 盘号 1, 盘符 "1:/"。
 *       SD1 没有卡检测脚(eMMC 焊死)。平台没使能 SD1 时自动退到 SD0 卡槽("0:/"),
 *       两条路的记录格式、命令、回读完全一样, 只是盘符不同。
 *
 * 文件: 一行一条记录, 纯逗号分隔 CSV(单位 ms / 0.001 A / 0.001 V / 0.001 mm/s / 0.001 mm,
 *       全是整数, 板上不做浮点):
 *           t_ms,i1,i2,i3,i4,i5,vbus,speed,x_mm,laser_mv
 *
 *       speed = 动子速度(毫米/秒, 取绝对值), x_mm = 动子位移(毫米),
 *       laser_mv = 激光引脚电压(毫伏, 排障用) —— 字段只许加在末尾, 老记录(9 列)在回读时
 *       会被 Emmc_IsDataLine 当成结构不对丢掉。
 *
 * 回读不做解析: 按行读回来原样交给上层, 上层拼前缀发走, 存的和发的格式永远一致。
 *
 * 线程安全: FatFs 默认不可重入, 本模块用一把递归互斥量把所有调用串起来
 *          (递归是必须的: Emmc_Truncate 会持锁再调别的函数)。
 * cache: SD 控制器不是缓存一致的, 内部扇区缓冲一律静态 + 32 字节对齐。
 *******************************************************************************
 */

#ifndef BSP_EMMC_H
#define BSP_EMMC_H

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/*  硬件绑定与常量                                                             */
/* -------------------------------------------------------------------------- */

/** 首选盘: eMMC 挂在 SD1 上(FatFs 逻辑盘号 = SD 实例号 = 1)。 */
#define EMMC_DRIVE              1U

/** 盘符(不能只写 "1:", f_open 要 "1:/x")。 */
#define EMMC_VOLUME             "1:/"

/** 数据文件(8.3 名字: xilffs 没开长文件名)。 */
#define EMMC_DATA_FILE          "1:/data.csv"

/** 退路: SD0 上的 SD 卡槽(MIO 40..45, 有卡检测脚)。 */
#define EMMC_DRIVE_ALT          0U
#define EMMC_VOLUME_ALT         "0:/"
#define EMMC_DATA_FILE_ALT      "0:/data.csv"

/** 数据文件第一行(表头), 回读时跳过; 单位与网口实时 DATA 行完全一致。 */
#define EMMC_DATA_HEADER        "t_ms,v1,v2,v3,v4,v5,vbus,speed,x_mm,laser_mv"

/** 一行 CSV 的最大长度(含 '\r\n' 和结尾 '\0')。 */
#define EMMC_LINE_MAX           128U

/** 回读时一次最多交给上层几行。 */
#define EMMC_READ_MAX_LINES     32U

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                     */
/* -------------------------------------------------------------------------- */

typedef enum {
    EMMC_OK             =  0,
    EMMC_ERR_PARAM      = -1,
    EMMC_ERR_STATE      = -2,   /**< 没挂载 / 文件没打开                        */
    EMMC_ERR_MOUNT      = -3,   /**< 挂载失败(卡没插 / 没格式化 / SD1 没起来)   */
    EMMC_ERR_IO         = -4,   /**< 读写失败(卡拔了 / 坏块)                    */
    EMMC_ERR_FULL       = -5,   /**< 卡满                                       */
    EMMC_ERR_NOTFOUND   = -6,   /**< 文件不存在                                 */
    EMMC_ERR_BUFFER     = -7    /**< 调用者给的缓冲装不下(不是硬件错)           */
} Emmc_StatusTypeDef;

/** 给上层看的只读快照。 */
typedef struct {
    uint8_t  Mounted;           /**< 1 = 挂载成功过(之后没掉)                   */
    uint8_t  FileOpen;          /**< 1 = 数据文件当前打开着                     */
    uint8_t  LastError;         /**< 最近一次 FatFs 返回码(0 = FR_OK)          */
    uint8_t  Reserved;
    uint32_t Records;           /**< 文件里现有的数据行条数(不含表头)           */
    uint32_t Bytes;             /**< 文件当前字节数                             */
    uint32_t FreeClusters;      /**< 剩余簇(挂载/查询空闲时刷新)               */
    uint32_t TotalClusters;     /**< 总簇                                      */
} Emmc_InfoTypeDef;

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief  挂载文件系统(盘符 "1:/", 不可用就退到 "0:/")。
 * @note   失败不是致命的: 没卡时别的功能照常跑。可以反复调(现场插卡后再挂一次)。
 */
Emmc_StatusTypeDef Emmc_Mount(void);

/** @brief  卸载(掉卡/重挂前先调), 会把数据文件关掉。 */
void Emmc_Unmount(void);

/** @brief  格式化成 FAT32 —— 会清掉卡上的东西(卡是新的或 f_mount 报 FR_NO_FILESYSTEM 时用)。 */
Emmc_StatusTypeDef Emmc_Format(void);

/**
 * @brief  打开数据文件准备追加。Rewind = 1 先清空(开始新采集), 0 接着往后写。
 * @note   Rewind = 0 时会扫一遍文件数出已有记录数; 大文件要在锁里待一会儿, 只做一次。
 */
Emmc_StatusTypeDef Emmc_OpenData(uint8_t Rewind);

/** @brief  关闭数据文件(先 f_sync 刷盘)。 */
Emmc_StatusTypeDef Emmc_CloseData(void);

/** @brief  数据文件当前是否打开着(0/1)。 */
uint8_t Emmc_DataIsOpen(void);

/** @brief  追加一段格式化好的文本(一段或几行); 同时更新快照的 Bytes / Records。 */
Emmc_StatusTypeDef Emmc_AppendText(const char *Text, uint32_t Len);

/** @brief  把文件系统缓冲刷到卡上(f_sync), 录像时周期性调。 */
Emmc_StatusTypeDef Emmc_Sync(void);

/** @brief  清空数据文件(重写表头)并重置计数。 */
Emmc_StatusTypeDef Emmc_Truncate(void);

/**
 * @brief  回读: 从第 Start 条数据行开始, 最多 MaxLines 行, 拷进 OutBuf。
 * @param  Prefix   : 每行前面加这个字符串(可为 NULL / ""), 上层用它拼回放行头。
 * @param  OutBuf   : 输出缓冲, 每行后面已补好 "\r\n"。
 * @param  OutLines : 输出: 实际拷了几行。
 * @param  More     : 输出: 这批之后还有没有记录(1 = 还有)。
 * @note   文本 CSV 没法随机定位, 实现是"从头顺序读、跳过前 Start 行", 所以要分批拉。
 */
Emmc_StatusTypeDef Emmc_ReadLines(uint32_t Start, uint32_t MaxLines,
                                  const char *Prefix,
                                  char *OutBuf, uint32_t OutSize,
                                  uint32_t *OutLines, uint32_t *OutLen,
                                  uint8_t *More);

/** @brief  读一份快照; Out == NULL 时什么都不做。 */
void Emmc_GetInfo(Emmc_InfoTypeDef *Out);

/* -------------------------------------------------------------------------- */
/*  当前用哪块盘                                                               */
/* -------------------------------------------------------------------------- */

/** @brief  当前实际在用的盘符("1:/" 或 "0:/"); 返回内部静态串, 别改别存。 */
const char *Emmc_GetVolume(void);

/** @brief  当前实际在用的 FatFs 逻辑盘号(1 = SD1/eMMC, 0 = SD0/卡槽)。 */
uint8_t Emmc_GetDrive(void);

/** @brief  本平台有没有 SD1(板载 eMMC)。 */
uint8_t Emmc_PlatformHasSd1Vol(void);

/** @brief  手动指定下一次用的盘(0 = 卡槽, 1 = eMMC); 只换盘符不挂载。 */
Emmc_StatusTypeDef Emmc_SelectVolume(uint8_t Drive);

#endif /* BSP_EMMC_H */
