/**
 *******************************************************************************
 * @file    ps_spi.h
 * @brief   PS SPI0 主机 + ADS8688 的 CS/DAISY/RST —— bsp 层
 *
 * 只有 service/ads(ADS8688)在用。
 *
 * 硬件(base_system_pins.xdc):
 *   SPI0 经 EMIO:  SCLK N15 / MOSI M17 / MISO N16(上拉输入)
 *   CS/DAISY/RST:  axi_gpio_1(0x41210000) 通道1 的 5 位输出 gpio_io_o[4:0]
 *                  bit0=CS->M18  bit1=DAISY->L14  bit2=RST->L15  bit3/4->L20/M14
 *                  通道2 是 1 位输入(gpio2_io_i[0] -> L19, 上拉, 备用)
 *
 * 三条不能改的:
 *   1. Mode 0 = 只开 XSPIPS_MASTER_OPTION, 不加 CPOL/CPHA;
 *   2. 控制器的 SS 一根都没引出来, 但初始化仍要选一路 SS(SetSlaveSelect(x, 0)),
 *      否则主机模式不移位 -> 传输超时; CS 由 AXI GPIO 拉;
 *   3. 一次 PsSpi_Transfer() = 一个 CS 周期 = 一帧(ADS8688 的要求)。
 *
 * 时钟: SPI 参考时钟 200 MHz(SLCR 0xF8000158 = IO PLL/5), SCLK = ref / 2^(N+1),
 *       默认 1.5625 MHz(N=6)。参考时钟来自 BSP(xspips_g.c), 改 BD 时钟要重建平台。
 *******************************************************************************
 */

#ifndef BSP_PS_SPI_H
#define BSP_PS_SPI_H

#include <stdint.h>
#include "xparameters.h"
#include "xspips.h"
#include "xgpio.h"

/** SPI0 控制器基地址(经 EMIO 引出后仍在 PS 内部: 0xE0006000)。 */
#define PS_SPI_DEV_BASEADDR     XPAR_XSPIPS_0_BASEADDR

/**
 * 控制器内部"用哪一路片选"的下标(0 = SS0)。
 * @note 和板上引脚无关(SS 没引出来, CS 由 AXI GPIO 拉), 但主机模式必须先告诉控制器
 *       选一路 SS, 否则不移位(SSCTRL 复位值 0xF = 不选), 现象是 TXOW 永远不来。
 */
#define PS_SPI_CS_INDEX         0U

/** CS/DAISY/RST 用的 AXI GPIO(BD 里是 axi_gpio_1)。 */
#ifdef XPAR_AXI_GPIO_1_BASEADDR
#define PS_SPI_GPIO_BASEADDR    XPAR_AXI_GPIO_1_BASEADDR
#else
#define PS_SPI_GPIO_BASEADDR    0x41210000U
#endif

#define PS_SPI_GPIO_CHANNEL     1U      /**< 5 位输出: CS/DAISY/RST/备用 */
#define PS_SPI_GPIO2_CHANNEL    2U      /**< 1 位输入: L19, 备用         */

/**
 * 目标 SCLK(Hz), 必须落在控制器分得出的档上, 否则会落到相邻一档:
 *   参考时钟 200 MHz, SCLK = ref / 2^(N+1):  N=5 -> 3.125 MHz  N=6 -> 1.5625 MHz
 *   现场读不出数就改这个宏重新烧(N=7 -> 781.25 kHz)。
 */
#define PS_SPI_TARGET_HZ        1562500U
#define PS_SPI_XFER_TIMEOUT_MS  50U         /**< 一帧超时(没有它可能卡死)  */

typedef enum {
    PS_SPI_OK = 0, PS_SPI_ERR_PARAM = -1, PS_SPI_ERR_INIT = -2, PS_SPI_ERR_BUSY = -3,
    PS_SPI_ERR_XFER = -4, PS_SPI_ERR_TIMEOUT = -6, PS_SPI_ERR_GPIO = -7
} PsSpi_StatusTypeDef;

/* -------------------------------------------------------------------------- */
/*  三根控制脚 —— CS / DAISY / RST, 一根一个对象                                */
/* -------------------------------------------------------------------------- */

/**
 * 一根控制脚的全部信息: 位置(哪片 GPIO / 哪一路 / 哪一位) + 有效电平 + 空闲电平 +
 * 名字 + 板上引脚号。PsSpi_Init() 会把这些打进上电日志, 认引脚不用再翻注释。
 */
typedef struct {
    uint32_t    BaseAddr;       /**< 挂在哪片 AXI GPIO(现在都是 axi_gpio_1)     */
    uint8_t     Channel;        /**< 哪一路(1 = 5 位输出通道)                    */
    uint8_t     Bit;            /**< 位号 -> gpio_io_o[Bit]                      */
    uint8_t     ActiveLevel;    /**< "起作用"时的电平(CS/RST 低有效)             */
    uint8_t     IdleLevel;      /**< 空闲电平(CS 高 / DAISY 低 / RST 高)         */
    const char *Name;           /**< "CS" / "DAISY" / "RST" / "SPARE0" / "SPARE1" */
    const char *BoardPin;       /**< 板上引脚(与 xdc 一致)                        */
} PsSpi_PinTypeDef;

/** 片选: M18, gpio_io_o[0], 低有效 —— 每帧一个脉冲。 */
extern const PsSpi_PinTypeDef PS_SPI_PIN_CS;
/** 菊花链输入: L14, gpio_io_o[1], 只能为低 —— 拉高会把器件带进菊花链模式。 */
extern const PsSpi_PinTypeDef PS_SPI_PIN_DAISY;
/** 复位: L15, gpio_io_o[2], 低有效 —— 除上电那一次脉冲外不动。 */
extern const PsSpi_PinTypeDef PS_SPI_PIN_RST;

/** 空闲输出字: CS 高 + DAISY 低 + RST 高 = 0x05(由三根脚的 IdleLevel 拼出, 用于自检)。 */
#define PS_SPI_CTRL_IDLE        0x05U

/** 上电硬复位脉冲的低电平宽度(µs) —— 数据手册要求 >400 ns, 这里给足。 */
#define PS_SPI_RST_LOW_US       10U

/** @brief  电平。 */
typedef enum { PS_SPI_LEVEL_LOW = 0, PS_SPI_LEVEL_HIGH = 1 } PsSpi_LevelTypeDef;

typedef struct {
    XSpiPs   Spi;                       /**< SPI 控制器                    */
    XGpio    Gpio;                      /**< AXI GPIO(CS/DAISY/RST)        */
    uint8_t  Ready;                     /**< 1 = 初始化完成                */
    uint8_t  GpioReady;                 /**< 1 = AXI GPIO 打开成功         */
    uint8_t  Prescaler;                 /**< 预分频值 N                    */
    uint8_t  Ctrl;                      /**< 输出字影子(bit0 CS/1 DAISY/2 RST) */
    uint32_t RefClkHz;                  /**< 参考时钟 Hz                   */
    uint32_t SclkHz;                    /**< 实际 SCLK Hz                  */
    uint32_t Transfers, TxBytes, RxBytes, Errors;
    volatile uint8_t Busy;              /**< 1 = 正在传输                  */
    int32_t  LastErr;                   /**< 最近错误码(0 = 正常)          */
} PsSpi_HandleTypeDef;

/** @brief  初始化: AXI GPIO + SPI0(主机 Mode 0) + RST 硬复位脉冲。 */
PsSpi_StatusTypeDef PsSpi_Init(PsSpi_HandleTypeDef *h, uint32_t baseAddr);

/** @brief  收发一帧(整帧共用一个 CS 周期); rx 可为 NULL。 */
PsSpi_StatusTypeDef PsSpi_Transfer(PsSpi_HandleTypeDef *h, const uint8_t *tx,
                                   uint8_t *rx, uint16_t len);

/* -------------------------------------------------------------------------- */
/*  三根控制脚各自的接口                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief  写任意一根控制脚(备用脚和以后新增的脚都走它)。
 * @note   往 DAISY 写高会被拒绝并打一行错误日志: 那会把 ADS8688 带进菊花链模式,
 *         命令全部写不进去。真要调菊花链就改这里。
 */
PsSpi_StatusTypeDef PsSpi_PinWrite(PsSpi_HandleTypeDef *h, const PsSpi_PinTypeDef *Pin,
                                   uint8_t Level);

/**
 * @brief  读某根控制脚当前的电平(输出寄存器镜像, 只证明软件/总线这一侧)。
 * @note   要验板上的线, 用跳线把 M18/L14/L15 接到 L19, 再看 PsSpi_ReadPins() 的 bit5。
 */
uint8_t PsSpi_PinRead(const PsSpi_HandleTypeDef *h, const PsSpi_PinTypeDef *Pin);

/** @brief  片选有效 —— 拉低 M18, 一帧的开头。 */
PsSpi_StatusTypeDef PsSpi_CsSelect(PsSpi_HandleTypeDef *h);

/** @brief  片选无效 —— 抬高 M18, 从机释放 SDO。 */
PsSpi_StatusTypeDef PsSpi_CsRelease(PsSpi_HandleTypeDef *h);

/** @brief  把 DAISY(L14)钉在低 —— 唯一安全的值, 上电与每次控制脚复位都要调。 */
PsSpi_StatusTypeDef PsSpi_DaisyHold(PsSpi_HandleTypeDef *h);

/** @brief  按住复位 —— 拉低 RST(L15)。 */
PsSpi_StatusTypeDef PsSpi_RstAssert(PsSpi_HandleTypeDef *h);

/** @brief  放开复位 —— 抬高 RST(L15)。 */
PsSpi_StatusTypeDef PsSpi_RstRelease(PsSpi_HandleTypeDef *h);

/** @brief  硬复位脉冲: 拉低 RST -> 等 LowUs 微秒 -> 放开; LowUs = 0 用默认值。 */
PsSpi_StatusTypeDef PsSpi_RstPulse(PsSpi_HandleTypeDef *h, uint32_t LowUs);

/** @brief  三根脚一起回空闲: CS 高 + DAISY 低 + RST 高(一次寄存器写, 无中间态)。 */
PsSpi_StatusTypeDef PsSpi_CtrlIdle(PsSpi_HandleTypeDef *h);

/** @brief  输出字当前值(bit0=CS, bit1=DAISY, bit2=RST; 空闲 = 0x05)。 */
uint8_t PsSpi_GetCtrlCode(const PsSpi_HandleTypeDef *h);

/** @brief  回读引脚: bit0..4 = 通道1 输出寄存器, bit5 = 通道2 输入脚(L19)。 */
uint8_t PsSpi_ReadPins(PsSpi_HandleTypeDef *h);

/** @brief  按参考时钟挑最接近目标频率的预分频值 N(SCLK = ref / 2^(N+1))。 */
uint8_t PsSpi_PrescalerFor(uint32_t refClkHz, uint32_t targetHz);

#endif /* BSP_PS_SPI_H */
