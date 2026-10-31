/**
 *******************************************************************************
 * @file    service_ads.c
 * @brief   ADS8688 8 通道采集: 配置器件 + 每 10 ms 扫一轮 + 换算成引脚电压 + 广播 DATA
 *
 * 帧格式(照 STM32 参考工程与厂商例程):
 *   命令帧   = 4 字节 [cmd_hi][cmd_lo][0x00][0x00](命令 + 再补一帧 NO_OP)
 *   写寄存器 = 2 字节 [(addr<<1)|1][data]
 *   读寄存器 = 3 字节 [addr<<1][0x00][0x00], 第 3 个收字节才是数据
 *   读转换值 = 4 字节: 先发 16 位 NO_OP, 后两字节才是结果(少这一帧会读到装载中的帧)
 *
 * 配置序列: 软复位(25 ms) -> 序列使能 -> 通道上电 -> 各通道量程 -> 回读 AUTO_SEQ_EN 校验。
 *
 * 读数方式由 service_ads.h 的 SVC_ADS_MANUAL_SCAN_DEFAULT 定型:
 *   1 = 单次扫描(默认): 每路先发 MAN_Ch_n 点名再读一帧, 一拍 16 帧, 不会序列错位;
 *   0 = 自动连续扫描: 器件按 AUTO_SEQ_EN 升序自己轮转, 配置里必须补 AUTO_RST。
 *
 * 硬件访问全经 bsp/ps_spi(一次 Transfer = 一个 CS 周期 = 一帧), 本文件不碰引脚。
 *******************************************************************************
 */

/* 上电配置回读、配置失败、传输失败这些一次性且关键的行都靠 LOG_*, 保持打开 */
#define LOG_ENABLE 1
#define LOG_TAG "ads"
#include "service/monitor_module/log/log.h"

#include "service/Samp_module/service_ads.h"
#include "service/Samp_module/ads8688.h"

#include "service/inter_module/net_text.h"

#include "bsp/ps_spi/ps_spi.h"      /* 本组件独占 PS SPI0(service -> bsp) */

#include "osal/osal.h"

#include <string.h>

/* ========================================================================== */
/*  现场标定表                                                                 */
/* ========================================================================== */

typedef struct {
    const char *Name;
    uint8_t     Range;          /**< 上电写给这一路的量程 ADS_RANGE_xxx          */
    uint8_t     IsLaser;        /**< 1 = 这一路接的是激光(填 DATA 行的"速度位")   */
} Ads_ChannelCfgTypeDef;

/** 通道 -> 物理量 对照表(换传感器 / 换接线只改这张表)。 */
static const Ads_ChannelCfgTypeDef gAdsCfg[SVC_ADS_CHANNELS] = {
    { "coil1", ADS_RANGE_UNIPOLAR_1P25REF, 0 },
    { "coil2", ADS_RANGE_UNIPOLAR_1P25REF, 0 },
    { "coil3", ADS_RANGE_UNIPOLAR_1P25REF, 0 },
    { "coil4", ADS_RANGE_UNIPOLAR_1P25REF, 0 },
    { "coil5", ADS_RANGE_UNIPOLAR_1P25REF, 0 },
    { "vbus",  ADS_RANGE_UNIPOLAR_1P25REF, 0 },
    /* CH6: 激光(只标记, 不折距离/速度) */
    { "laser", ADS_RANGE_UNIPOLAR_1P25REF, 1 },
    /* CH7: 备用 */
    { "spare", ADS_RANGE_UNIPOLAR_1P25REF, 0 },
};

/* ========================================================================== */
/*  组件参数                                                                   */
/* ========================================================================== */

/* "采哪几路"和"一轮读几帧"都归 service_ads.h 的 SVC_ADS_SEQ_MASK 管: 帧数由掩码现算,
 * 这里不要再定义一遍(两处各写一个值 → 自动扫描时会永久错位)。 */

/** 母线电压在标定表里的行号(CH5)。 */
#define SVC_ADS_VBUS_ROW        5U

/**
 * 线圈电压 -> 线圈电流的标定: I[mA] = 15 * (V[mV] - 零点[mV])。
 *   由 I[A] = (1.5 * V[V] - 2.5) * 10 展开: 零点 1.667 V, 灵敏度 15 A/V。
 *   零点默认 1667 mV, 可用 0xE3 命令在线标定(见 Svc_Ads_RequestZeroCalib)。
 *   换电流传感器 / 换采样电阻只改这两个数。
 */
#define SVC_ADS_COIL_MA_PER_MV  15
#define SVC_ADS_COIL_ZERO_MV    1667    /* 出厂零点(1.667 V), 每路可在线标定 */

/** 母线电压分压比: 母线电压 = CH5 引脚电压 * 11(板上是 11:1 分压)。 */
#define SVC_ADS_VBUS_DIVIDER    11

/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

static PsSpi_HandleTypeDef     gSpi;        /**< PS SPI0 主机句柄(本组件独占)   */
static Osal_MutexHandleTypeDef gSpiLock;    /**< 总线锁: 一轮扫描 <-> 一次配置事务 */

static uint8_t  gReady;
static uint8_t  gConfigured;                /**< 1 = 初始化序列回读校验通过      */

/* 采样与广播开关(由 0xE0 / 0xE1 决定): gRunning = 每 10 ms 扫一轮, gStreaming = 每拍广播 */
static uint8_t  gRunning   = (uint8_t)SVC_ADS_RUN_DEFAULT;
static uint8_t  gStreaming = (uint8_t)SVC_ADS_STREAM_DEFAULT;
static uint8_t  gSeqMask   = (uint8_t)SVC_ADS_SEQ_MASK;
static uint8_t  gSeqReadback;               /**< AUTO_SEQ_EN 的回读值            */

/** 各通道当前量程码(上电取自 gAdsCfg[])。 */
static uint8_t  gRanges[SVC_ADS_CHANNELS];

/** 这一轮要读哪几路: 由 gSeqMask 现算的升序表。 */
static uint8_t  gScanList[SVC_ADS_CHANNELS];
static uint8_t  gScanCount;

static uint32_t gScans;                     /**< 完成的整轮扫描次数              */
static uint32_t gErrors;                    /**< SPI 事务失败次数                */
static uint32_t gScanFails;                 /**< 扫描失败的拍数                  */
static uint32_t gBroadcasts;                /**< 发出的遥测行数                  */

/** 1 = 单次扫描(MAN_Ch_n 点名), 0 = 自动连续扫描(AUTO_RST)。 */
static uint8_t  gManualScan = (uint8_t)SVC_ADS_MANUAL_SCAN_DEFAULT;

/** 每拍样本的订阅者(录 eMMC 那条链)。NULL = 没人订阅。 */
static Svc_Ads_SampleSinkTypeDef gSink;

static uint16_t gRaw[SVC_ADS_CHANNELS];
static int32_t  gMilliVolts[SVC_ADS_CHANNELS];

static int32_t  gCoilMilliAmps[SVC_ADS_COILS]; /**< 线圈 1..5 的电流(mA)     */

/* 零点漂移补偿: 每路"零电流时对应的引脚电压"(mV)。出厂 1667, 可用 0xE3 在线标定。 */
static int32_t  gCoilZeroMv[SVC_ADS_COILS];

/* 零点标定状态机: 请求后累加 N 轮的引脚电压, 累够了取平均装进 gCoilZeroMv。 */
static volatile uint8_t  gZeroReq;                  /**< 1 = 有标定请求              */
static volatile uint8_t  gZeroBusy;                 /**< 1 = 正在累加                */
static uint32_t          gZeroRound;                /**< 已累加轮数                  */
static uint32_t          gZeroRounds;               /**< 本次要累加多少轮            */
static int32_t           gZeroSum[SVC_ADS_COILS];   /**< 各路电压累加和(mV)          */

/* 这两个名字沿用上位机 DATA 行的旧叫法:
 *   VbusMilliVolts = 母线电压(mV) = CH5 引脚电压 * 11
 *   SpeedMilliMmS  = 激光测速(毫 mm/s) —— 2026/10 起不再是"CH6 引脚电压", 见 Svc_Ads_LaserUpdate() */
static int32_t  gVbusMilliVolts;
static int32_t  gSpeedMilliMmS;

/* ---- 激光位移/测速的状态机 ---------------------------------------------- *
 * 判定阈值和换算参数都在 service_ads.h 的"激光位移标定"那一块, 现场改那里。
 */
static uint8_t  gLaserState;                /**< SVC_ADS_LASER_STATE_LOST / _TRACK   */
static uint8_t  gLaserDebounce;             /**< 已经连续几次满足"另一个状态"        */
static int32_t  gLaserMilliVolts;           /**< 这一拍 CH6 的引脚电压(mV)          */
static int32_t  gLaserMilliMm;              /**< 当前位移(毫 mm); 丢失时保持上一有效值 */
static uint8_t  gLaserHasLast;              /**< 1 = 上一拍有有效位移(可以差分)      */

/* 平滑用的滤波器状态(浮点, 不要换成整数, 否则细小的变化会被截断掉, 反而更不平滑) */
static float    gLaserXFilteredMm;          /**< 位移低通后的值(mm)                  */
static float    gLaserLastFilteredMm;       /**< 上一拍低通后的位移(mm), 差分用它     */
static float    gLaserSpeedFiltered;        /**< 速度低通后的值(毫 mm/s)             */
static uint8_t  gLaserFilterSeed;           /**< 0 = 滤波器还没初值, 下一拍直接装实测值 */

/* 前置声明: 零点标定的累加函数定义在后面, 但采集任务里要调它 */
static void Svc_Ads_ZeroAccumulate(void);

/* 前置声明: 激光状态机在换算段后面定义 */
static void Svc_Ads_LaserUpdate(int32_t MilliVolts, uint8_t Sampled);

/* ========================================================================== */
/*  总线锁                                                                     */
/* ========================================================================== */

/* 采集任务每 10 ms 发一轮帧, 配置序列在别的上下文发起 —— 两边的帧不能在总线上交错
 * (ADS8688 没有帧同步头)。句柄为 NULL 时 osal 是空操作。 */
static void Svc_Ads_Lock(void)
{
    Osal_MutexLock(gSpiLock);
}

static void Svc_Ads_Unlock(void)
{
    Osal_MutexUnlock(gSpiLock);
}

/* ========================================================================== */
/*  SPI 小工具(帧格式见文件头)                                                 */
/* ========================================================================== */

/** @brief  收发一帧(片选由 bsp 包在帧外: 一帧 = 一个 CS 周期)。 */
static int Svc_Ads_Xfer(const uint8_t *Tx, uint8_t *Rx, uint16_t Len)
{
    return (PsSpi_Transfer(&gSpi, Tx, Rx, Len) == PS_SPI_OK) ? 1 : 0;
}

/** @brief  发一条命令帧(4 字节)。 */
static int Svc_Ads_SendCmd(uint16_t Cmd)
{
    uint8_t frame[4];

    Ads_FrameCmd(Cmd, frame);

    return Svc_Ads_Xfer(frame, NULL, 4U);
}

/** @brief  写一个程序寄存器(2 字节)。 */
static int Svc_Ads_WriteReg(uint8_t Addr, uint8_t Data)
{
    uint8_t frame[2];

    Ads_FrameWriteReg(Addr, Data, frame);

    return Svc_Ads_Xfer(frame, NULL, 2U);
}

/** @brief  读一个程序寄存器(3 字节事务, 取第 3 个收字节)。 */
static int Svc_Ads_ReadReg(uint8_t Addr, uint8_t *Out)
{
    uint8_t tx[3];
    uint8_t rx[3];

    if (Out == NULL) {
        return 0;
    }

    Ads_FrameReadReg(Addr, tx);
    (void)memset(rx, 0, sizeof(rx));

    if (Svc_Ads_Xfer(tx, rx, 3U) == 0) {
        return 0;
    }

    *Out = rx[2];                   /* 第 3 个收字节才是寄存器值 */

    return 1;
}

/** @brief  读寄存器, 并把 3 个收字节原样带出来(排查取错字节用)。 */
static int Svc_Ads_ReadRegRaw(uint8_t Addr, uint8_t Raw[3], uint8_t *Out)
{
    uint8_t tx[3];

    if (Raw == NULL) {
        return 0;
    }

    Ads_FrameReadReg(Addr, tx);
    (void)memset(Raw, 0, 3U);

    if (Svc_Ads_Xfer(tx, Raw, 3U) == 0) {
        return 0;
    }

    if (Out != NULL) {
        *Out = Raw[2];
    }

    return 1;
}

/** @brief  读一次转换结果(4 字节事务, 后两字节才是结果); 自动模式下每读一次取回下一路。 */
static int Svc_Ads_ReadData(uint16_t *Raw)
{
    uint8_t tx[4] = { 0x00U, 0x00U, 0x00U, 0x00U };
    uint8_t rx[4];

    if (Raw == NULL) {
        return 0;
    }

    (void)memset(rx, 0, sizeof(rx));

    if (Svc_Ads_Xfer(tx, rx, 4U) == 0) {
        gErrors++;
        return 0;
    }

    *Raw = Ads_Be16(&rx[2]);

    return 1;
}

/** @brief  三根控制脚回到空闲(电平由 bsp/ps_spi 的引脚表决定)。 */
static void Svc_Ads_CtrlIdle(void)
{
    (void)PsSpi_CtrlIdle(&gSpi);
}

/* ========================================================================== */
/*  初始化序列(软复位 -> 序列使能 -> 通道上电 -> 量程 -> 回读校验)              */
/* ========================================================================== */

/**
 * @brief  让器件进入自动扫描: 重写序列与通道上电, 最后发 AUTO_RST。
 *
 * AUTO_RST 才是"进入自动扫描"的那条命令(只写 AUTO_SEQ_EN 器件不会轮转);
 * 它之后器件从最小的使能通道开始升序转换, 所以"一轮读几帧"必须等于掩码里 1 的个数。
 *
 * @return 1 = 成功; 0 = 某一帧没发出去。调用者持总线锁。
 */
static int Svc_Ads_StartAutoScan(void)
{
    if (Svc_Ads_WriteReg(ADS_REG_AUTO_SEQ_EN, gSeqMask) == 0) {
        LOG_E("auto scan: AUTO_SEQ_EN write failed\r\n");

        return 0;
    }

    if (Svc_Ads_WriteReg(ADS_REG_CH_PD, (uint8_t)(~gSeqMask)) == 0) {
        LOG_E("auto scan: CH_PD write failed\r\n");

        return 0;
    }

    if (Svc_Ads_SendCmd(ADS_CMD_AUTO_RST) == 0) {
        LOG_E("auto scan: AUTO_RST frame failed\r\n");

        return 0;
    }

    return 1;
}

/**
 * @brief  下发配置并回读校验。
 * @return 1 = AUTO_SEQ_EN 的回读值等于写下去的值。重试次数由 SVC_ADS_INIT_TRIES 封顶。
 */
static int Svc_Ads_Configure(void)
{
    uint8_t attempt;
    uint8_t ch;

    for (attempt = 0U; attempt < (uint8_t)SVC_ADS_INIT_TRIES; attempt++) {
        /* ---- 1. 软复位, 复位后等器件 ---- */
        if (Svc_Ads_SendCmd(ADS_CMD_RST) == 0) {
            LOG_E("configure step 1 failed: soft reset frame (spi xfer)\r\n");
            return 0;
        }
        Osal_DelayMs(25U);

        /* ---- 2. 自动扫描序列使能 ---- */
        if (Svc_Ads_WriteReg(ADS_REG_AUTO_SEQ_EN, gSeqMask) == 0) {
            LOG_E("configure step 2 failed: AUTO_SEQ_EN write (spi xfer)\r\n");
            return 0;
        }

        /* ---- 3. 通道上电(写 1 = 掉电, 所以写掩码取反) ---- */
        if (Svc_Ads_WriteReg(ADS_REG_CH_PD, (uint8_t)(~gSeqMask)) == 0) {
            LOG_E("configure step 3 failed: CH_PD write (spi xfer)\r\n");
            return 0;
        }
        Osal_DelayMs(50U);

        /* ---- 4. 各通道量程 ---- */
        for (ch = 0U; ch < SVC_ADS_CHANNELS; ch++) {
            if (Svc_Ads_WriteReg(ADS_REG_CHIR(ch), gRanges[ch]) == 0) {
                LOG_E("configure step 4 failed at channel %u: range write\r\n",
                      (unsigned)ch);
                return 0;
            }
        }

        /* ---- 5. 回读校验 ---- */
        gSeqReadback = 0U;
        if ((Svc_Ads_ReadReg(ADS_REG_AUTO_SEQ_EN, &gSeqReadback) != 0) &&
            (gSeqReadback == gSeqMask)) {
            /* ---- 6. 自动模式: 补一条 AUTO_RST 让器件开始轮转(手动模式不用发) ---- */
            if ((gManualScan == 0U) && (Svc_Ads_StartAutoScan() == 0)) {
                LOG_E("configure step 6 failed: AUTO_RST (spi xfer)\r\n");

                return 0;
            }

            gConfigured = 1U;
            return 1;
        }

        LOG_W("configure attempt %u: AUTO_SEQ_EN readback 0x%02X (want 0x%02X)\r\n",
              (unsigned)(attempt + 1U), (unsigned)gSeqReadback, (unsigned)gSeqMask);
        Osal_DelayMs(50U);
    }

    gConfigured = 0U;

    return 0;
}

/** @brief  把所有程序寄存器读回来打两行(初始化命令的验收单)。 */
static void Svc_Ads_DumpRegs(void)
{
    uint8_t seq  = 0U;
    uint8_t chpd = 0U;
    uint8_t fs   = 0U;
    uint8_t chir[SVC_ADS_CHANNELS];
    uint8_t ch;

    (void)memset(chir, 0, sizeof(chir));

    (void)Svc_Ads_ReadReg(ADS_REG_AUTO_SEQ_EN, &seq);
    (void)Svc_Ads_ReadReg(ADS_REG_CH_PD, &chpd);
    (void)Svc_Ads_ReadReg(ADS_REG_F_S, &fs);

    for (ch = 0U; ch < SVC_ADS_CHANNELS; ch++) {
        (void)Svc_Ads_ReadReg(ADS_REG_CHIR(ch), &chir[ch]);
    }

    LOG_I("regs: AUTO_SEQ_EN=0x%02X (write 0x%02X) CH_PD=0x%02X F_S=0x%02X\r\n",
          (unsigned)seq, (unsigned)gSeqMask, (unsigned)chpd, (unsigned)fs);
    LOG_I("regs CHIR0..7: %u %u %u %u %u %u %u %u\r\n",
          (unsigned)chir[0], (unsigned)chir[1], (unsigned)chir[2], (unsigned)chir[3],
          (unsigned)chir[4], (unsigned)chir[5], (unsigned)chir[6], (unsigned)chir[7]);

    if (seq != gSeqMask) {
        uint8_t raw[3];

        (void)Svc_Ads_ReadRegRaw(ADS_REG_AUTO_SEQ_EN, raw, NULL);

        /* 原样打出 3 个收字节: 全 FF = SDO 一直高(CS/MISO/供电), 全 00 = 没时钟或一直被复位 */
        LOG_W("readback mismatch: raw bytes %02X %02X %02X "
              "(FF FF FF = SDO high/CS not reaching the part, 00 00 00 = no clock or held in reset)\r\n",
              (unsigned)raw[0], (unsigned)raw[1], (unsigned)raw[2]);
    }
}

/* ========================================================================== */
/*  扫描                                                                       */
/* ========================================================================== */

/** @brief  这一路在序列掩码里吗(决定要不要读、要不要上电、要不要换算)。 */
static int Svc_Ads_Enabled(uint8_t Ch)
{
    return ((gSeqMask & ADS_CH_BIT(Ch)) != 0U) ? 1 : 0;
}

/** @brief  标定表里"激光"那一行的通道号(找不到返回 SVC_ADS_CHANNELS)。 */
static uint8_t Svc_Ads_LaserChannel(void)
{
    uint8_t ch;

    for (ch = 0U; ch < SVC_ADS_CHANNELS; ch++) {
        if (gAdsCfg[ch].IsLaser != 0U) {
            return ch;
        }
    }

    return (uint8_t)SVC_ADS_CHANNELS;
}

/**
 * @brief  由序列掩码现算"这一轮读哪几路"(升序)。
 *
 * 自动扫描时器件按升序轮转, 所以读的次序和帧数都必须从掩码现算, 不能另外配一个数。
 */
static void Svc_Ads_BuildScanList(void)
{
    uint8_t ch;

    gScanCount = 0U;

    for (ch = 0U; ch < SVC_ADS_CHANNELS; ch++) {
        if (Svc_Ads_Enabled(ch) != 0) {
            gScanList[gScanCount] = ch;
            gScanCount++;
        }
    }

    if (gScanCount == 0U) {
        /* 掩码全 0 = 一路都不采(配错了): 兜底成第一路, 免得上层永远收不到数 */
        LOG_W("scan mask 0x%02X enables no channel - falling back to ch0\r\n",
              (unsigned)gSeqMask);
        gScanList[0] = 0U;
        gScanCount   = 1U;
    }
}

/** @brief  自动连续扫描: 连读 gScanCount 帧, 第 i 帧就是序列里第 i 路(需先发 AUTO_RST)。 */
static int Svc_Ads_ScanAuto(uint16_t *Raw)
{
    uint8_t i;

    for (i = 0U; i < gScanCount; i++) {
        if (Svc_Ads_ReadData(&Raw[gScanList[i]]) == 0) {
            return 0;
        }
    }

    return 1;
}

/** @brief  单次扫描(默认): 对每一路先发 MAN_Ch_n 点名, 再读一帧结果。 */
static int Svc_Ads_ScanManual(uint16_t *Raw)
{
    uint8_t i;

    for (i = 0U; i < gScanCount; i++) {
        const uint8_t ch = gScanList[i];

        /* 1. 选通道 */
        if (Svc_Ads_SendCmd(ADS_CMD_MAN_CH(ch)) == 0) {
            return 0;
        }

        /* 2. 读结果(后两字节) */
        if (Svc_Ads_ReadData(&Raw[ch]) == 0) {
            return 0;
        }
    }

    return 1;
}

/** @brief  按当前模式扫一轮。 @return 1 = 每一帧都收发正常。调用者持总线锁。 */
static int Svc_Ads_Scan(uint16_t *Raw)
{
    if (Raw == NULL) {
        return 0;
    }

    return (gManualScan != 0U) ? Svc_Ads_ScanManual(Raw) : Svc_Ads_ScanAuto(Raw);
}

/* ========================================================================== */
/*  换算                                                                       */
/* ========================================================================== */

/**
 * @brief  激光状态机: 引脚电压 -> 丢失/跟踪 -> 位移(mm) + 速度(mm/s)。
 *
 * 判定表(阈值都是 mV, 在 service_ads.h 里改):
 *
 *     V < 300            丢失(无回波): 不折位移、不算速度, 速度报 0
 *     300 <= V < 380     过渡防抖区: **状态保持不变**, 只当"还没确认"来数防抖次数
 *     380 <= V <= 2020   有效跟踪区: 折位移, 两拍都有效才算速度
 *     2020 < V <= 2100   上限余量区: 同过渡区, 状态保持不变(不切换、不出新位移)
 *     V > 2100           丢失(超上限)
 *
 * 防抖: 只有连续 SVC_ADS_LASER_DEBOUNCE_N 次都想切到**同一个**状态才真的切, 压制
 *       ADC 噪声在阈值附近的来回跳。
 *
 * 测速(强制规则): 只有**上一拍 AND 当前拍**都在 TRACK 才算 Δx/Δt —— 任何一次状态
 *       跳变(丢失->有效、有效->丢失)都清掉位置缓存, 于是"跳变后的第一拍"永远报 0,
 *       不会拿跨越丢失段的两点算出一个假速度。
 *
 * 位移换算(两点直线插值, 手册 4.5.2 的式子):
 *       x[mm] = x1 + (x2 - x1) / (y2 - y1) * (V[V] - y1)
 *   现在: 0.732 V -> 0 mm, 1.143 V -> 480 mm。结果存成毫 mm(int32)。
 *
 * @param  MilliVolts : 这一拍 CH6 的引脚电压(mV)。
 * @param  Sampled    : 1 = 这一拍真的采到了 CH6; 0 = 序列里没使能它, 按丢失处理。
 */
static void Svc_Ads_LaserUpdate(int32_t MilliVolts, uint8_t Sampled)
{
    uint8_t want;
    int32_t xMilli;
    float   spanV;
    float   xMm;

    gLaserMilliVolts = MilliVolts;

    /* ---- 1. 这一拍"应该"是什么状态 ---- */
    if (Sampled == 0U) {
        /* 没采到这个通道: 手上的电压是旧的, 不能拿它冒充, 直接判丢失(也不防抖) */
        if (gLaserState != (uint8_t)SVC_ADS_LASER_STATE_LOST) {
            LOG_I("laser -> LOST (channel not sampled)\r\n");
        }
        gLaserState    = (uint8_t)SVC_ADS_LASER_STATE_LOST;
        gLaserDebounce = 0U;
        gSpeedMilliMmS = 0;
        gLaserHasLast  = 0U;
        gLaserFilterSeed = 0U;           /* 滤波器也要重置, 下次重新装实测值 */
        return;
    }

    if (MilliVolts < (int32_t)SVC_ADS_LASER_LOST_MV) {
        want = (uint8_t)SVC_ADS_LASER_STATE_LOST;        /* 无回波 */
    } else if (MilliVolts > (int32_t)SVC_ADS_LASER_LOST_MAX_MV) {
        want = (uint8_t)SVC_ADS_LASER_STATE_LOST;        /* 超上限 */
    } else if ((MilliVolts >= (int32_t)SVC_ADS_LASER_DEBOUNCE_MV) &&
               (MilliVolts <= (int32_t)SVC_ADS_LASER_TRACK_MAX_MV)) {
        want = (uint8_t)SVC_ADS_LASER_STATE_TRACK;       /* 有效跟踪区 */
    } else {
        /* 过渡防抖区(300..380) 和上限余量区(2020..2100): 都不切换状态 */
        want = gLaserState;
    }

    /* ---- 2. 防抖: 连续 N 次都想要同一个新状态才切 ---- */
    if (want == gLaserState) {
        gLaserDebounce = 0U;
    } else {
        if (gLaserDebounce < 0xFFU) {
            gLaserDebounce++;
        }

        if (gLaserDebounce >= (uint8_t)SVC_ADS_LASER_DEBOUNCE_N) {
            gLaserDebounce = 0U;
            gLaserState    = want;
            gLaserHasLast  = 0U;         /* 跳变: 位置缓存作废, 下一拍不许算速度 */
            gLaserFilterSeed = 0U;       /* 滤波器也重新起头, 免得把丢失段的旧值拖进来 */

            /* 这里只报电压: 刚跳变的这一拍位移还没重算, 打出来的是上一段的旧值 */
            LOG_I("laser -> %s (V=%ld mV)\r\n",
                  (want == (uint8_t)SVC_ADS_LASER_STATE_TRACK) ? "TRACK" : "LOST",
                  (long)MilliVolts);
        }
    }

    /* ---- 3. 状态动作 ---- */
    if (gLaserState != (uint8_t)SVC_ADS_LASER_STATE_TRACK) {
        gSpeedMilliMmS = 0;              /* 丢失: 速度必须是 0 */
        gLaserHasLast  = 0U;             /* 位置缓存作废(丢失段不许参与差分) */
        gLaserFilterSeed = 0U;           /* 滤波器也复位 */
        return;
    }

    /* ---- 4. 跟踪: 电压 -> 位移(毫 mm), 两点直线插值 ---- */
    spanV = (float)SVC_ADS_LASER_Y2_V - (float)SVC_ADS_LASER_Y1_V;

    if (spanV < 0.0f) {
        spanV = -spanV;                  /* 标定填反了也别把位移算成相反的斜率 */
    }

    if (spanV < 0.001f) {
        /* y1 == y2: 标定没填好, 这一拍不给位移(保持上一拍), 免得除 0 出乱数 */
        return;
    }

    xMm = (float)SVC_ADS_LASER_X1_MM
        + (((float)SVC_ADS_LASER_X2_MM - (float)SVC_ADS_LASER_X1_MM) / spanV)
        * (((float)MilliVolts / 1000.0f) - (float)SVC_ADS_LASER_Y1_V);

    /* ---- 4b. 位移低通: 先滤位移, 后面用滤波后的位移去差分 ---- */
    if (gLaserFilterSeed == 0U) {
        /* 刚进跟踪区(或滤波被复位): 直接装实测值, 不让滤波器从 0 慢慢爬上来 */
        gLaserXFilteredMm    = xMm;
        gLaserLastFilteredMm = xMm;
        gLaserSpeedFiltered  = 0.0f;
        gLaserFilterSeed     = 1U;
    } else if ((float)SVC_ADS_LASER_DISP_ALPHA < 1.0f) {
        gLaserXFilteredMm += (float)SVC_ADS_LASER_DISP_ALPHA * (xMm - gLaserXFilteredMm);
    } else {
        gLaserXFilteredMm = xMm;         /* alpha = 1: 不滤波 */
    }

    xMilli = (int32_t)((gLaserXFilteredMm * 1000.0f) +
                       ((gLaserXFilteredMm >= 0.0f) ? 0.5f : -0.5f));   /* 四舍五入 */
    gLaserMilliMm = xMilli;

    /* ---- 5. 测速: 上一拍也有效才算(用的是低通后的位移) ---- */
    if (gLaserHasLast != 0U) {
        const float dxMilli = (gLaserXFilteredMm - gLaserLastFilteredMm) * 1000.0f;   /* 微米 */
        float       v = (dxMilli * 1000.0f) / (float)SVC_ADS_LASER_DT_MS;             /* 毫mm/s */

        /* 默认报速率(取绝对值): 位移减小时 Δx 是负的, 直接发出去曲线就是负的谷 */
        if (SVC_ADS_LASER_SPEED_ABS != 0) {
            if (v < 0.0f) {
                v = -v;
            }
        }

        /* 速度再轻滤一次(差分对噪声最敏感) */
        if ((float)SVC_ADS_LASER_SPEED_ALPHA < 1.0f) {
            gLaserSpeedFiltered += (float)SVC_ADS_LASER_SPEED_ALPHA * (v - gLaserSpeedFiltered);
        } else {
            gLaserSpeedFiltered = v;
        }

        v = gLaserSpeedFiltered;

        /*
         * 死区: 动子停住时位移只剩 ADC 那 ±1 个 LSB 的抖动, 差分之差还是十几 mm/s,
         * 而且速度取了绝对值(零均值噪声被整流成正的), 曲线就是一条贴 0 的毛带。
         * 低于死区直接报 0, 静止段就干干净净。
         */
        if (v < ((float)SVC_ADS_LASER_SPEED_DEADBAND_MMS * 1000.0f)) {
            v = 0.0f;
            gLaserSpeedFiltered = 0.0f;      /* 顺手把滤波器也压住, 免得一走动就带出残留 */
        }

        gSpeedMilliMmS = (int32_t)(v + 0.5f);
    } else {
        gSpeedMilliMmS       = 0;        /* 刚进跟踪区: 这一拍没有上一拍可比 */
        gLaserSpeedFiltered  = 0.0f;
    }

    gLaserLastFilteredMm = gLaserXFilteredMm;
    gLaserHasLast        = 1U;
}

/**
 * @brief  原始码 -> 电压(mV) -> 线圈电流(mA) / 母线电压(mV) / 激光位移+速度。
 *
 * 8 路同一个电压算法(Ads_RawToMilliVolts + 该路自己的量程), 得到**引脚电压**;
 * CH6 再走状态机折成位移和速度(见 Svc_Ads_LaserUpdate)。
 *
 * 再做两步换算:
 *   线圈 1..5: I[mA] = 15 * V[mV] - 25000(见组件参数里的标定);
 *   母线 CH5 : Vbus[mV] = 引脚电压 * 11(11:1 分压)。
 * 没被序列掩码使能的通道报 0(表示"没采"), 不按 0 V 折算成 -25 A。
 *
 * DATA 行的字段位置沿用旧契约: 线圈 1..5(电流), 第 6 个(速度位) = 激光速度,
 * 第 7 个(母线位) = 母线电压, 第 8 个(新增, 只能加在末尾) = 激光位移。
 * 所以这一行的通道顺序是 ch0..ch4, ch6, ch5 + 位移。
 */
static void Svc_Ads_Convert(void)
{
    uint8_t i;
    uint8_t ch;

    /* ---- 1. 采到的每一路: 原始码 -> 引脚电压(mV) ---- */
    for (i = 0U; i < gScanCount; i++) {
        ch = gScanList[i];
        gMilliVolts[ch] = Ads_RawToMilliVolts(gRaw[ch], gRanges[ch]);
    }

    /* ---- 2. 线圈 1..5: (引脚电压 - 该路零点) -> 电流(mA) ---- */
    for (ch = 0U; ch < (uint8_t)SVC_ADS_COILS; ch++) {
        if ((gSeqMask & (uint8_t)(1U << ch)) != 0U) {
            const int32_t mv = gMilliVolts[ch] - gCoilZeroMv[ch];
            int32_t       ma = (int32_t)SVC_ADS_COIL_MA_PER_MV * mv;

            /* 死区: 零点附近的噪声(±1~2 个 LSB)直接归 0, 界面/曲线才不会一直抖 */
            if ((ma > -(int32_t)SVC_ADS_COIL_DEADBAND_MA) &&
                (ma <  (int32_t)SVC_ADS_COIL_DEADBAND_MA)) {
                ma = 0;
            }

            gCoilMilliAmps[ch] = ma;
        } else {
            gCoilMilliAmps[ch] = 0;
        }
    }

    /* ---- 3. 母线: CH5 引脚电压 * 11*/
    gVbusMilliVolts = gMilliVolts[SVC_ADS_VBUS_ROW] * (int32_t)SVC_ADS_VBUS_DIVIDER;

    ch = Svc_Ads_LaserChannel();                      /* 标定表里"激光"那一行 */

    /* 激光: 电压 -> 状态机 -> 位移(mm) + 速度(mm/s); 没使能这一路就按丢失处理 */
    Svc_Ads_LaserUpdate(((ch < (uint8_t)SVC_ADS_CHANNELS) ? gMilliVolts[ch] : 0),
                        (((ch < (uint8_t)SVC_ADS_CHANNELS) &&
                          ((gSeqMask & (uint8_t)(1U << ch)) != 0U)) ? 1U : 0U));
}

/* ========================================================================== */
/*  输出: 遥测广播(DATA 行) / 样本订阅                                          */
/* ========================================================================== */

/** @brief  追加一个千分之一单位的数, 带 3 位小数(2503 -> "2.503")。 */
static uint16_t Svc_Ads_AppendMilli(char *Dst, uint16_t Pos, uint16_t Size, int32_t Milli)
{
    uint32_t mag;

    if (Milli < 0) {
        Pos = Svc_Text_AppendStr(Dst, Pos, Size, "-");
        mag = (uint32_t)(-(int64_t)Milli);
    } else {
        mag = (uint32_t)Milli;
    }

    Pos = Svc_Text_AppendUint(Dst, Pos, Size, mag / 1000U);
    Pos = Svc_Text_AppendStr(Dst, Pos, Size, ".");

    {
        const uint32_t frac = mag % 1000U;

        if (frac < 100U) {
            Pos = Svc_Text_AppendStr(Dst, Pos, Size, "0");
        }
        if (frac < 10U) {
            Pos = Svc_Text_AppendStr(Dst, Pos, Size, "0");
        }
        Pos = Svc_Text_AppendUint(Dst, Pos, Size, frac);
    }

    return Pos;
}

/**
 * @brief  广播一行遥测: "DATA <秒> <i1>..<i5> <动子速度> <母线电压> <动子位移> <激光电压>"。
 *
 * 第 1 个数是秒, 接下来 5 个是线圈电流(A), 第 7 个是激光测速(mm/s),
 * 第 8 个是母线电压(V, 已按 11:1 还原), 第 9 个是激光位移(mm, 2026/10 新增),
 * 第 10 个是**激光引脚电压(V, 2026/11 新增, 只给排障看)**。
 * 都是 3 位小数(1 mA / 1 mV / 1 mm)。字段位置是固定契约, 不随掩码变。
 * ⚠ 新增字段只能加在**末尾**(插在中间老上位机会整体错位)。
 *
 * 为什么要把原始电压也发出来: 速度/位移都是从它折出来的, 曲线一有怪形状, 第一件事就是
 * 看电压是不是也那样 —— 电压单调而 mm 不单调 = 换算问题; 电压自己就回升/跳变 = 机械
 * 回弹、光斑问题, 或者中途丢过目标(丢目标时电压会掉到 300 mV 附近或冲到 2100 mV)。
 */
static void Svc_Ads_Broadcast(uint32_t NowMs)
{
    char     text[SVC_NET_MSG_MAX];
    uint16_t pos = 0U;
    uint8_t  ch;

    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "DATA ");
    pos = Svc_Ads_AppendMilli(text, pos, (uint16_t)sizeof(text), (int32_t)NowMs);

    for (ch = 0U; ch < (uint8_t)SVC_ADS_COILS; ch++) {
        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " ");
        pos = Svc_Ads_AppendMilli(text, pos, (uint16_t)sizeof(text), gCoilMilliAmps[ch]);
    }

    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " ");
    pos = Svc_Ads_AppendMilli(text, pos, (uint16_t)sizeof(text), gSpeedMilliMmS);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " ");
    pos = Svc_Ads_AppendMilli(text, pos, (uint16_t)sizeof(text), gVbusMilliVolts);

    /* 第 9 个数: 激光位移(mm)。丢失目标时这里保持上一拍的有效值(速度已经是 0) */
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " ");
    pos = Svc_Ads_AppendMilli(text, pos, (uint16_t)sizeof(text), gLaserMilliMm);

    /* 第 10 个数: 激光引脚电压(V) —— 排障用, 上位机不画它 */
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " ");
    pos = Svc_Ads_AppendMilli(text, pos, (uint16_t)sizeof(text), gLaserMilliVolts);
    pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "\r\n");

    /* 队列满就丢这一行, 采集节拍不受影响(丢的行数在 net 快照里) */
    if (Svc_Net_SendText(text, pos) == SVC_OK) {
        gBroadcasts++;
    }
}

/** @brief  把这一拍交给订阅者(录 eMMC 那条链)。 */
static void Svc_Ads_PublishSample(uint32_t NowMs)
{
    const Svc_Ads_SampleSinkTypeDef sink = gSink;

    if (sink != NULL) {
        Svc_Ads_SampleTypeDef s;

        s.UptimeMs       = NowMs;
        s.VbusMilliVolts = gVbusMilliVolts;
        s.SpeedMilliMmS  = gSpeedMilliMmS;
        s.LaserMilliMm   = gLaserMilliMm;
        s.LaserMilliVolts = gLaserMilliVolts;
        (void)memcpy(s.CoilMilliAmps, gCoilMilliAmps, sizeof(s.CoilMilliAmps));

        sink(&s);                       /* 实现必须是"拷走就返回", 见 service_ads.h */
    }
}

/* ========================================================================== */
/*  任务函数                                                                   */
/* ========================================================================== */

/**
 * @brief  ads 任务: 上电配置一次, 之后等 0xE0 才开始每 10 ms 采一拍。
 *
 * 采集失败不退出循环: 打一行、计数 +1, 下一拍接着试。
 */
void Svc_Ads_Task(void *Arg)             /* 创建由应用层负责, 见 service_ads.h */
{
    uint32_t now;

    (void)Arg;

    /* 配置序列带几十毫秒延时, 所以放在任务里下发 */
    Svc_Ads_Lock();
    Svc_Ads_CtrlIdle();
    Svc_Ads_Configure();
    Svc_Ads_DumpRegs();
    Svc_Ads_Unlock();

    if (gConfigured != 0U) {
        LOG_I("configured: seq mask 0x%02X (readback 0x%02X), %s scan\r\n",
              (unsigned)gSeqMask, (unsigned)gSeqReadback,
              (gManualScan != 0U) ? "manual" : "auto");
    } else {
        LOG_E("configure failed: readback 0x%02X (want 0x%02X) - check CS/SCLK/MOSI/MISO\r\n",
              (unsigned)gSeqReadback, (unsigned)gSeqMask);
    }

    if (SVC_ADS_INIT_ONLY != 0U) {
        /* 只跑初始化命令, 之后总线保持安静 */
        LOG_W("INIT-ONLY mode: no scanning, no broadcast - the bus stays quiet\r\n");

        for (;;) {
            Osal_DelayMs(1000U);
        }
    }

    for (;;) {
        int ok;

        /* 没收到 0xE0 之前什么都不做: 不发 SPI、不广播、不送样本 */
        if (gRunning == 0U) {
            Osal_DelayMs(SVC_ADS_PERIOD_MS);
            continue;
        }

        now = Osal_UptimeMs();

        Svc_Ads_Lock();
        ok = Svc_Ads_Scan(gRaw);
        Svc_Ads_Unlock();

        if (ok != 0) {
            gScans++;
            Svc_Ads_Convert();
            Svc_Ads_ZeroAccumulate();   /* 换算完就累加零点(和广播用同一拍数据) */
        } else {
            gScanFails++;       /* 单帧失败次数由 ReadData 的 gErrors 数 */
        }

        /* 先给订阅者, 再按需广播 */
        Svc_Ads_PublishSample(now);

        if (gStreaming != 0U) {
            Svc_Ads_Broadcast(now);
        }

        Osal_DelayMs(SVC_ADS_PERIOD_MS);
    }
}

/* ========================================================================== */
/*  采样开关(命令在 app/samp_app.c, 这里只提供能力)                             */
/* ========================================================================== */

/**
 * @brief  开始/停止采样。
 *
 * 开 = 每 10 ms 扫一轮 + 每拍广播 DATA + 给 eMMC 送样本;
 * 停 = 一次 SPI 事务都不发(总线安静), 不广播、不送样本。
 */
Svc_StatusTypeDef Svc_Ads_SetRunning(int On)
{
    gRunning   = (On != 0) ? 1U : 0U;
    gStreaming = (On != 0) ? 1U : 0U;

    LOG_I("sampling %s\r\n", (On != 0) ? "ON" : "OFF (bus quiet)");

    return SVC_OK;
}

int Svc_Ads_IsRunning(void)
{
    return (gRunning != 0U) ? 1 : 0;
}

int Svc_Ads_IsStreaming(void)
{
    return (gStreaming != 0U) ? 1 : 0;
}

/* ========================================================================== */
/*  对外接口                                                                   */
/* ========================================================================== */

/** @brief  零点标定: 累加当前引脚电压, 够轮数就取平均写进 gCoilZeroMv[]。 */
static void Svc_Ads_ZeroAccumulate(void)
{
    uint8_t  ch;
    uint8_t  i;
    int32_t  out[SVC_ADS_COILS];

    /* 1. 有新请求: 开始一轮新的累加 */
    if (gZeroReq != 0U) {
        gZeroReq   = 0U;
        gZeroBusy  = 1U;
        gZeroRound = 0U;

        for (ch = 0U; ch < (uint8_t)SVC_ADS_COILS; ch++) {
            gZeroSum[ch] = 0;
        }

        LOG_I("zero calibration: averaging %u rounds (brake must be OFF)\r\n",
              (unsigned)gZeroRounds);
    }

    if (gZeroBusy == 0U) {
        return;
    }

    /* 2. 累加这一拍(只累加真正采到的通道) */
    for (i = 0U; i < gScanCount; i++) {
        ch = gScanList[i];
        if (ch < (uint8_t)SVC_ADS_COILS) {
            gZeroSum[ch] += gMilliVolts[ch];
        }
    }

    gZeroRound++;

    if (gZeroRound < gZeroRounds) {
        return;
    }

    /* 3. 够轮数了: 取平均, 这就是"零电流时这一路的电压" */
    for (ch = 0U; ch < (uint8_t)SVC_ADS_COILS; ch++) {
        gCoilZeroMv[ch] = gZeroSum[ch] / (int32_t)gZeroRounds;
        out[ch]         = gCoilZeroMv[ch];
    }

    gZeroBusy = 0U;

    /* 串口 + 网口都报一遍: 现场要能看到这 5 个归零电压 */
    LOG_I("zero calibrated: ch0..4 = %d %d %d %d %d mV -> current reads 0 A at rest\r\n",
          (int)out[0], (int)out[1], (int)out[2], (int)out[3], (int)out[4]);
    {
        char     text[SVC_NET_MSG_MAX];
        uint16_t pos = 0U;

        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "ads zero (mV):");
        for (ch = 0U; ch < (uint8_t)SVC_ADS_COILS; ch++) {
            pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " ");
            pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text),
                                      (uint32_t)((out[ch] < 0) ? 0 : out[ch]));
        }
        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "\r\n");
        (void)Svc_Net_SendText(text, pos);
    }
}

Svc_StatusTypeDef Svc_Ads_RequestZeroCalib(uint32_t Rounds)
{
    if (gRunning == 0U) {
        return SVC_ERR_STATE;              /* 没在采样就没有电压可采 */
    }

    if (Rounds == 0U) {
        Rounds = SVC_ADS_ZERO_ROUNDS;
    }
    if (Rounds > SVC_ADS_ZERO_ROUNDS_MAX) {
        Rounds = SVC_ADS_ZERO_ROUNDS_MAX;
    }

    gZeroRounds = Rounds;
    gZeroReq    = 1U;

    return SVC_OK;
}

int Svc_Ads_IsZeroCalibrating(void)
{
    return (gZeroBusy != 0U) ? 1 : 0;
}

/** @brief  读 5 路线圈电流(mA) —— 电流闭环拿这个当反馈。 */
Svc_StatusTypeDef Svc_Ads_GetCoilCurrentMa(int32_t *Out, uint32_t Count)
{
    uint32_t ch;

    if ((Out == NULL) || (Count == 0U)) {
        return SVC_ERR_PARAM;
    }

    for (ch = 0U; (ch < (uint32_t)SVC_ADS_COILS) && (ch < Count); ch++) {
        Out[ch] = gCoilMilliAmps[ch];
    }

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Ads_GetZeroMv(int32_t *Out, uint32_t Count)
{
    uint32_t ch;

    if ((Out == NULL) || (Count == 0U)) {
        return SVC_ERR_PARAM;
    }

    for (ch = 0U; (ch < (uint32_t)SVC_ADS_COILS) && (ch < Count); ch++) {
        Out[ch] = gCoilZeroMv[ch];
    }

    return SVC_OK;
}

void Svc_Ads_GetLaserState(uint8_t *State, int32_t *MilliVolts,
                           int32_t *MilliMm, int32_t *SpeedMilliMmS)
{
    if (State != NULL) {
        *State = gLaserState;
    }
    if (MilliVolts != NULL) {
        *MilliVolts = gLaserMilliVolts;
    }
    if (MilliMm != NULL) {
        *MilliMm = gLaserMilliMm;
    }
    if (SpeedMilliMmS != NULL) {
        *SpeedMilliMmS = gSpeedMilliMmS;
    }
}

Svc_StatusTypeDef Svc_Ads_Init(void)
{
    uint8_t ch;
    uint8_t i;

    for (ch = 0U; ch < SVC_ADS_CHANNELS; ch++) {
        gRanges[ch] = gAdsCfg[ch].Range;
    }

    /* 线圈零点先装出厂值(1.667 V); 现场用 0xE3 采一次"当前电压"覆盖它 */
    for (ch = 0U; ch < (uint8_t)SVC_ADS_COILS; ch++) {
        gCoilZeroMv[ch] = (int32_t)SVC_ADS_COIL_ZERO_MV;
    }

    /* "采哪几路"从掩码现算(改 service_ads.h 的 SVC_ADS_SEQ_MASK 即可) */
    Svc_Ads_BuildScanList();

    /* 激光状态机先摆在"丢失": 没采到有效电压之前不许当跟踪, 也不许算速度 */
    gLaserState       = (uint8_t)SVC_ADS_LASER_STATE_LOST;
    gLaserDebounce    = 0U;
    gLaserMilliVolts  = 0;
    gLaserMilliMm     = 0;
    gLaserHasLast     = 0U;
    gSpeedMilliMmS    = 0;

    /* 平滑滤波器: 第一次进跟踪区时直接装实测值(见 Svc_Ads_LaserUpdate) */
    gLaserXFilteredMm    = 0.0f;
    gLaserLastFilteredMm = 0.0f;
    gLaserSpeedFiltered  = 0.0f;
    gLaserFilterSeed     = 0U;

    gSpiLock = Osal_MutexCreateRecursive();     /* 建不起来也不致命: NULL 时锁是空操作 */

    if (PsSpi_Init(&gSpi, PS_SPI_DEV_BASEADDR) != PS_SPI_OK) {
        LOG_E("PS SPI0 init failed\r\n");

        return SVC_ERR_HW;
    }

    gReady = 1U;

    /* 打一行三根控制脚当前的电平, 方便和接线核对(引脚号/空闲电平的真值在 bsp) */
    LOG_I("ready: PS SPI0 SCLK %u Hz (N=%u), ctrl 0x%02X => CS(%s)=%u DAISY(%s)=%u RST(%s)=%u\r\n",
          (unsigned)gSpi.SclkHz, (unsigned)gSpi.Prescaler,
          (unsigned)PsSpi_GetCtrlCode(&gSpi),
          PS_SPI_PIN_CS.BoardPin,    (unsigned)PsSpi_PinRead(&gSpi, &PS_SPI_PIN_CS),
          PS_SPI_PIN_DAISY.BoardPin, (unsigned)PsSpi_PinRead(&gSpi, &PS_SPI_PIN_DAISY),
          PS_SPI_PIN_RST.BoardPin,   (unsigned)PsSpi_PinRead(&gSpi, &PS_SPI_PIN_RST));
    LOG_I("channel map: ch0..4 = coil1..5, ch5 = vbus, ch6 = laser, ch7 = spare (values in mV)\r\n");

    /* 打一行"现在到底采哪几路" */
    {
        char     text[SVC_NET_MSG_MAX];
        uint16_t pos;

        pos = Svc_Text_AppendStr(text, 0U, (uint16_t)sizeof(text), "scan mask 0x");
        pos = Svc_Text_AppendHex2(text, pos, (uint16_t)sizeof(text), gSeqMask);
        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " -> ");
        pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text), (uint32_t)gScanCount);
        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " channel(s) per round:");

        for (i = 0U; i < gScanCount; i++) {
            pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), " ch");
            pos = Svc_Text_AppendUint(text, pos, (uint16_t)sizeof(text),
                                      (uint32_t)gScanList[i]);
            pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "(");
            pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text),
                                     gAdsCfg[gScanList[i]].Name);
            pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), ")");
        }

        pos = Svc_Text_AppendStr(text, pos, (uint16_t)sizeof(text), "\r\n");
        LOG_I("%s", text);
    }

    return SVC_OK;
}

uint8_t Svc_Ads_IsReady(void)
{
    return gReady;
}

Svc_StatusTypeDef Svc_Ads_SetSampleSink(Svc_Ads_SampleSinkTypeDef Sink)
{
    gSink = Sink;       /* 单次写指针是原子的 */

    return SVC_OK;
}
