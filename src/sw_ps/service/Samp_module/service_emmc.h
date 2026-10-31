/**
 *******************************************************************************
 * @file    service_emmc.h
 * @brief   把 ADS 的样本录到 eMMC(CSV) 并提供录制状态
 *
 * 数据流: ADS 任务(10 ms) -> Svc_Emmc_OnSample() -> 队列 -> emmc 任务
 *         -> 拼 CSV 行 -> 攒够 512 B 写盘 -> bsp/emmc -> FatFs -> SD1(eMMC)
 *
 *   队列: 写盘要几毫秒到几十毫秒, 不能拖住 10 ms 的采样节拍; 满了丢样本并计数。
 *   暂存: 攒满一个扇区再写, f_write 的调用次数少一个数量级。
 *
 * 盘上格式(CSV): t_ms 从本次录制起点的 0 开始, i1..i5 是线圈电流(A),
 * vbus 是母线电压(V, 已按 11:1 还原), speed 是 CH6 的引脚电压(V), 都保留 3 位小数:
 *     t_ms,i1,i2,i3,i4,i5,vbus,speed
 *
 * 录制是自动的: 卡挂上 + 采样在跑就录, 没有开关命令。
 *******************************************************************************
 */

#ifndef SERVICE_EMMC_H
#define SERVICE_EMMC_H

#include <stdint.h>

#include "service/service.h"
#include "service/Samp_module/service_ads.h"     /* Svc_Ads_SampleTypeDef(样本来源) */
#include "bsp/emmc/emmc.h"                       /* Emmc_InfoTypeDef(快照里带出去)  */

/* -------------------------------------------------------------------------- */
/*  组件参数                                                                   */
/* -------------------------------------------------------------------------- */

#define SVC_EMMC_STACK_WORDS    1536U   /**< 6 KB: FatFs 内部调用链栈帧较深        */
#define SVC_EMMC_PRIO           2U      /**< 比 ADS(3) 低: 写盘不许抢采样节拍      */
#define SVC_EMMC_Q_LEN          128U    /**< 100 Hz 下一拍 10 ms, 128 条 ≈ 1.3 s  */

/** 攒多少字节写一次盘(一个扇区)。 */
#define SVC_EMMC_STAGE_BYTES    512U

/** 多久 f_sync 一次(掉电最多丢这么久的数据)。 */
#define SVC_EMMC_SYNC_MS        1000U

/** 任务等队列的超时, 也是"没样本时多久轮一次刷盘/同步"的节拍。 */
#define SVC_EMMC_POLL_MS        200U

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                     */
/* -------------------------------------------------------------------------- */

/** 录制状态快照。 */
typedef struct {
    Emmc_InfoTypeDef Disk;          /**< 卡/文件状态(来自 bsp/emmc)   */
    uint8_t          Recording;     /**< 1 = 正在录                   */
    uint8_t          Started;       /**< 1 = 组件起来了               */
    uint16_t         Reserved;
    uint32_t         Samples;       /**< 收到的样本总数               */
    uint32_t         Written;       /**< 写进文件的记录数             */
    uint32_t         Dropped;       /**< 队列满丢掉的样本数           */
    uint32_t         Errors;        /**< 写盘失败次数                 */
    uint32_t         Syncs;         /**< f_sync 次数                  */
    uint32_t         T0Ms;          /**< 本次录制起点(uptime), 0 = 未开始 */
} Svc_Emmc_SnapshotTypeDef;

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief  挂载 eMMC + 建队列/互斥量/暂存缓冲。挂载只在这里做一次(开机后才插卡要重启)。
 * @return SVC_OK / SVC_ERR_HW(队列或锁分配失败)。没卡不算错, 应用照常跑。
 */
Svc_StatusTypeDef Svc_Emmc_Init(void);

/** @brief  初始化好了没有(1 = 队列/锁都建起来了)。 */
uint8_t Svc_Emmc_IsReady(void);

/** @brief  任务入口(样本出队 + 攒够 512 B 写盘 + 每秒 sync); 任务由应用层创建。 */
void Svc_Emmc_Task(void *Arg);

/** @brief  样本入口(挂给 Svc_Ads_SetSampleSink; 跑在 ADS 任务里, 只入队就返回)。 */
void Svc_Emmc_OnSample(const Svc_Ads_SampleTypeDef *Sample);

/** @brief  读一份快照。 */
Svc_StatusTypeDef Svc_Emmc_GetSnapshot(Svc_Emmc_SnapshotTypeDef *Out);

#endif /* SERVICE_EMMC_H */
