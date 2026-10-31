/**
 *******************************************************************************
 * @file    ls.h
 * @brief   光幕输入模块 — 6 路 LS_I 输入(PS EMIO GPIO, bank2)
 *
 * 硬件: PL 外部端口 LS_I[5:0] -> processing_system7_0 / GPIO_I(EMIO GPIO)
 *   LS_I[0] -> W15    LS_I[1] -> V12    LS_I[2] -> W13
 *   LS_I[3] -> V15    LS_I[4] -> R19    LS_I[5] -> P19  (bank34, LVCMOS33 + 上拉)
 *   EMIO 位 i 对应 PS GPIO 引脚号 54+i。
 *
 * 电平: 引脚配了 PULLUP, 光幕输出低有效 —— 没遮挡 = 高, 被遮挡 = 被拉低。
 *       模块对外的状态位 1 表示"该路被遮挡"。
 *
 * 消抖: Ls_Scan() 每调一次算一次采样, 同一个新电平连续出现 LS_DEBOUNCE_SCANS 次
 *       才认为状态真的变了, 用来滤光幕毛刺和机械抖动。
 *
 * 用法(由调用者周期调用):
 *   Ls_Init(&h, LS_GPIO_BASEADDR, LS_ACTIVE_LOW);
 *   for (;;) { Ls_Scan(&h, &changed); Osal_DelayMs(5); }
 *******************************************************************************
 */

#ifndef LS_H
#define LS_H

#include <stdint.h>

#include "xgpiops.h"
#include "xparameters.h"

/* -------------------------------------------------------------------------- */
/* 硬件绑定                                                                    */
/* -------------------------------------------------------------------------- */

/** PS GPIO 控制器基地址(0xE000A000)。 */
#define LS_GPIO_BASEADDR        XPAR_XGPIOPS_0_BASEADDR

/** 第一路对应的 PS GPIO 引脚号(EMIO 位 0)。 */
#define LS_PIN_FIRST            54U

/** 光幕路数, 必须和 PL 里 LS_I 的宽度一致。 */
#define LS_CHANNELS             6U

/** 6 路都有效的位掩码。 */
#define LS_MASK                 ((uint8_t)((1U << LS_CHANNELS) - 1U))

/** 1 = 被遮挡时引脚为低(默认, XDC 里有上拉)。 */
#define LS_ACTIVE_LOW           1U

/** 连续采样多少次才确认状态变化。 */
#define LS_DEBOUNCE_SCANS       3U

/* -------------------------------------------------------------------------- */
/* 类型                                                                        */
/* -------------------------------------------------------------------------- */

typedef enum {
    LS_OK           =  0,       /**< 成功                                  */
    LS_ERR_PARAM    = -1,       /**< 空指针 / 参数越界                     */
    LS_ERR_HW       = -2        /**< GPIO 控制器打不开                     */
} Ls_StatusTypeDef;

/**
 * @brief  光幕状态。State 是消抖后的对外状态(1 = 遮挡), Raw 是最近一次读到的
 *         原始电平(1 = 高), 都按 bit0..bit5 对应 LS_I[0..5]。
 */
typedef struct {
    XGpioPs  Gpio;                      /**< XGpioPs 驱动实例             */
    uint8_t  Ready;                     /**< 1 = 初始化完成               */
    uint8_t  ActiveLow;                 /**< 1 = 遮挡为低电平             */
    uint8_t  Debounce;                  /**< 消抖次数(>=1)                */
    uint8_t  Raw;                       /**< 最近一次的原始电平           */
    uint8_t  State;                     /**< 消抖后的状态, 1 = 遮挡       */
    uint8_t  Count[LS_CHANNELS];        /**< 每路的连续相同采样计数       */
    uint32_t Scans;                     /**< Ls_Scan() 次数               */
    uint32_t Changes;                   /**< 状态变化次数                 */
} Ls_HandleTypeDef;

/* -------------------------------------------------------------------------- */
/* 公共接口                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief  打开 PS GPIO 并把 6 根 LS_I 配成输入(baseAddr = 0 用 LS_GPIO_BASEADDR)。
 * @note   结束时读一次当前电平作为初始状态, 所以上电瞬间不算一次变化。
 */
Ls_StatusTypeDef Ls_Init(Ls_HandleTypeDef *h, uint32_t baseAddr, uint8_t activeLow);

/** @brief  立刻读一次引脚, 不做消抖; 返回原始电平位(1 = 高)。 */
uint8_t Ls_ReadRaw(Ls_HandleTypeDef *h);

/** @brief  采样一次并做消抖; changedMask = 状态真的变化了的位(可为 NULL)。 */
uint8_t Ls_Scan(Ls_HandleTypeDef *h, uint8_t *changedMask);

/** @brief  最近一次消抖后的状态(1 = 遮挡)。 */
uint8_t Ls_GetState(const Ls_HandleTypeDef *h);

/** @brief  最近一次读到的原始电平(1 = 高)。 */
uint8_t Ls_GetRaw(const Ls_HandleTypeDef *h);

/** @brief  累计采样次数(诊断用)。 */
uint32_t Ls_GetScans(const Ls_HandleTypeDef *h);

/** @brief  把位掩码格式化成 "1,3,5"; 掩码 0 输出 "-"(建议缓冲 >= 24 字节)。 */
void Ls_FormatMask(uint8_t mask, char *dst, uint16_t size);

/** @brief  打印一行状态(状态位 + 原始电平 + 变化次数)。 */
void Ls_PrintStatus(const Ls_HandleTypeDef *h);

#endif /* LS_H */
