/**
 *******************************************************************************
 * @file    led.h
 * @brief   breath_led IP 驱动(4 路呼吸灯, AXI 基址 0x43C00000)
 *
 * 引脚(base_system_pins.xdc):
 *   LED0 -> K16(网口在线指示用它)   LED1 -> J16   LED2 -> K14   LED3 -> J14
 *
 * 寄存器:
 *   offset 0x00  CTRL   bit[3:0] = LED0..LED3 使能, 1 = 呼吸, 0 = 灭
 *   offset 0x04  PERIOD [9:0] = freq_set, IP 夹到 10..1000
 *
 * 四路共用同一个呼吸频率(硬件只有一组 ramp 计数器), 每颗灯只有"灭"或
 * "按共享频率呼吸"两种状态。没有亮度寄存器。
 * 亮度每 1 ms 增减 freq_set 个计数, 呼吸周期 = 2*50000/freq_set ms:
 *   freq = 10 -> ~10 s    freq = 100 -> ~1 s    freq = 1000 -> ~100 ms
 *
 * CTRL 向后兼容: 以前只有 bit0, 现在 bit[3:0] 是四路使能, bit0 含义没变。
 *******************************************************************************
 */

#ifndef LED_H
#define LED_H

#include <stdint.h>
#include <stddef.h>

#include "xparameters.h"

/* -------------------------------------------------------------------------- */
/* Hardware binding                                                           */
/* -------------------------------------------------------------------------- */

#define LED_BASEADDR                XPAR_BREATH_LED_0_BASEADDR

#define LED_REG_CTRL                0x00U   /**< bit[3:0] = 四路使能        */
#define LED_REG_PERIOD              0x04U   /**< [9:0] = 共享呼吸频率       */

/* -------------------------------------------------------------------------- */
/* 四路 LED 的编号 / 掩码                                                      */
/* -------------------------------------------------------------------------- */

/** 板上的呼吸灯路数(= IP 的 led 位宽)。 */
#define LED_COUNT                   4U

/** 四路全亮的掩码。 */
#define LED_MASK_ALL                ((uint8_t)((1U << LED_COUNT) - 1U))

/** 指示灯那一颗(LED0 / K16): 网口在线指示等"系统状态"用它。 */
#define LED_INDEX_INDICATOR         0U
#define LED_MASK_INDICATOR          ((uint8_t)(1U << LED_INDEX_INDICATOR))

/** 某一路的掩码(Index 必须是编译期常量, 不检查越界)。 */
#define LED_MASK_OF(Index)          ((uint8_t)(1U << (Index)))

/* -------------------------------------------------------------------------- */
/* Types                                                                      */
/* -------------------------------------------------------------------------- */

typedef enum {
    LED_OK          =  0,
    LED_ERR_PARAM   = -1,
    LED_ERR_STATE   = -2
} Led_StatusTypeDef;

/** 预定义模式(数值是 RS-485 协议定的, 不要改)。 */
typedef enum {
    LED_MODE_OFF            = 0,    /**< 灭                                   */
    LED_MODE_BREATH_SLOW    = 1,    /**< ~5 s 呼吸                            */
    LED_MODE_BREATH_NORMAL  = 2,    /**< ~1 s 呼吸                            */
    LED_MODE_BREATH_FAST    = 3,    /**< ~100 ms 呼吸                         */
    LED_MODE_COUNT          = 4
} Led_ModeTypeDef;

/** IP 的频率范围(越界会被硬件夹住)。 */
#define LED_FREQ_MIN                10U
#define LED_FREQ_MAX                1000U

/* -------------------------------------------------------------------------- */
/* Handle                                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t        BaseAddr;   /**< breath_led AXI 基址                     */
    uint8_t         Enabled;    /**< 1 = 至少有一路亮                        */
    uint8_t         Mask;       /**< CTRL 的影子: bit[3:0] = LED0..3 使能     */
    Led_ModeTypeDef Mode;       /**< 最后一次请求的模式                       */
    uint16_t        Freq;       /**< 最后一次设的频率                         */
    uint32_t        Writes;     /**< 寄存器写次数                             */
} Led_HandleTypeDef;

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/** @brief  绑定句柄并关灯(base = 0 表示用 LED_BASEADDR)。 */
Led_StatusTypeDef Led_Init(Led_HandleTypeDef *h, uint32_t base);

/** @brief  一次设置四路使能掩码; 只动 CTRL, 不动共享频率。 */
Led_StatusTypeDef Led_SetMask(Led_HandleTypeDef *h, uint8_t Mask);

/** @brief  开关其中一路(Index = 0..3), 其它三路保持原样。 */
Led_StatusTypeDef Led_SetEnable(Led_HandleTypeDef *h, uint8_t Index, uint8_t On);

/** @brief  读回当前的使能掩码(bit[3:0])。 */
uint8_t Led_GetMask(const Led_HandleTypeDef *h);

/**
 * @brief  套用预定义模式。
 *
 * 对掩码的影响:
 *   LED_MODE_OFF                -> 掩码清零(四路全灭)
 *   BREATH_SLOW/NORMAL/FAST     -> 只改共享频率; 掩码为 0 时点亮指示灯(LED0),
 *                                  已经有灯亮着就原样保留(不会关掉用户点亮的其它路)
 */
Led_StatusTypeDef Led_SetMode(Led_HandleTypeDef *h, Led_ModeTypeDef mode);

/**
 * @brief  直接设共享呼吸频率(10..1000, 会被夹住)。
 * @note   四路共用这一个频率。设频率的同时会打开 CTRL: 内核不使能时周期完全无效。
 *         掩码为 0 时点亮指示灯, 否则按当前掩码重写 CTRL。
 */
Led_StatusTypeDef Led_SetFreq(Led_HandleTypeDef *h, uint16_t freq);

/** @brief  读回模块认为的状态(mode / freq / enabled 都可以传 NULL); 掩码用 Led_GetMask()。 */
Led_StatusTypeDef Led_GetState(const Led_HandleTypeDef *h, Led_ModeTypeDef *mode,
                               uint16_t *freq, uint8_t *enabled);

/** @brief  模式对应的频率(未知模式返回 LED_FREQ_MIN)。 */
uint16_t Led_ModeToFreq(Led_ModeTypeDef mode);

/** @brief  最接近某频率的模式(直接把频率告诉对端时用)。 */
Led_ModeTypeDef Led_FreqToMode(uint16_t freq);

/** @brief  模式名("off" / "slow" / "normal" / "fast")。 */
const char *Led_ModeName(Led_ModeTypeDef mode);

#endif /* LED_H */
