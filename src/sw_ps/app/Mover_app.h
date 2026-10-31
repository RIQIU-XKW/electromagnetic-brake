/**
 *******************************************************************************
 * @file    Mover_app.h
 * @brief   发射小车: 单字节命令 0xD0..0xD6 + 整周期时序(由本模块任务推进)
 *
 *   0xD0          发射(用当前档位)      0xD1..0xD6  选 SS1..SS6 档并立刻发射
 *   档位会记住: 之后发 0xD0 就用最后选的那个档
 *   回话一行文本: "D3 launch 700 mm/s armed / BUSY / ERR"
 *
 * 时序(每次发射): 初始化三条 -> 去程使能 -> 速度 -> 发射位 -> 启动 ->
 *                回程使能 -> 位置 0 -> 启动 -> 停机, 整周期约 5.5 s。
 * 发帧与 CiA402 动作都调 service/Mover_module 的 Svc_Motor_*。
 *******************************************************************************
 */

#ifndef MOVER_APP_H
#define MOVER_APP_H

#include <stdint.h>

#include "service/service.h"
#include "service/inter_module/service_net.h"     /* Svc_Net_CmdTypeDef / Svc_Net_ReplyTypeDef */

/* -------------------------------------------------------------------------- */
/*  命令码                                                                     */
/* -------------------------------------------------------------------------- */

#define MOVER_CMD_LAUNCH        0xD0U   /**< 发射(当前档位)                    */
#define MOVER_CMD_GEAR_1        0xD1U   /**< SS1 档 + 发射                     */
#define MOVER_CMD_GEAR_6        0xD6U   /**< SS6 档 + 发射                     */

/* -------------------------------------------------------------------------- */
/*  档位(SS1..SS6: 0.5 m/s 起, 每档 +0.1 m/s)                                   */
/* -------------------------------------------------------------------------- */

#define MOVER_GEAR_FIRST_MMPS   500L    /**< SS1 速度(mm/s)                    */
#define MOVER_GEAR_STEP_MMPS    100L    /**< 每档增量                          */
#define MOVER_GEAR_COUNT        6U
#define MOVER_GEAR_DEFAULT      2U      /**< 上电默认档 = SS3(700 mm/s)        */

/* -------------------------------------------------------------------------- */
/*  时序时间点(ms) —— 要改发射节奏就改这几个数                                   */
/* -------------------------------------------------------------------------- */

#define MOVER_INIT_SETTLE_MS    1500U   /**< 初始化三条之后等驱动器稳定         */
#define MOVER_ENABLE_SETTLE_MS  200U    /**< 使能(0x6040)之后                  */
#define MOVER_SPEED_SETTLE_MS   100U    /**< 速度/位置写完到启动之间            */
#define MOVER_LAUNCH_RUN_MS     1500U   /**< 去程运行时间                      */
#define MOVER_RETURN_MS         1800U   /**< 回程运行时间                      */

/** 发射位写进 0x607A 的原始值(单位是脉冲, 现场校准后填)。 */
#define MOVER_LAUNCH_POS        400000L

/* -------------------------------------------------------------------------- */
/*  任务参数                                                                   */
/* -------------------------------------------------------------------------- */

#define MOVER_TASK_STACK_WORDS  512U
#define MOVER_TASK_PRIO         2U      /**< 低于 net/485/ads/brake, 高于 monitor */

/* -------------------------------------------------------------------------- */
/*  状态(内部变量)                                                                 */
/* -------------------------------------------------------------------------- */

typedef enum {
    MOVER_SEQ_IDLE = 0,         /**< 没在跑(上电初值)                     */
    MOVER_SEQ_ARM,              /**< 已挂上, 等任务发初始化那三条          */
    MOVER_SEQ_INIT_WAIT,        /**< 初始化已发, 等驱动器稳定              */
    MOVER_SEQ_ENABLE_WAIT,      /**< 去程使能(0x000F)已发, 等一会儿        */
    MOVER_SEQ_SPEED_WAIT,       /**< 速度(0x6081)已发, 等一会儿            */
    MOVER_SEQ_OUT_GO_WAIT,      /**< 发射位(0x607A)已写, 等一下再启动       */
    MOVER_SEQ_OUT_RUN,          /**< 去程启动(0x001F)已发, 车在跑          */
    MOVER_SEQ_BACK_ENABLE_WAIT, /**< 回程使能已发, 等一会儿                */
    MOVER_SEQ_BACK_POS_WAIT,    /**< 目标位置 0 已写, 等一下再启动          */
    MOVER_SEQ_BACK_RUN,         /**< 回程启动已发, 车往原点跑              */
    MOVER_SEQ_DONE              /**< 停机(0x0006)已发, 整周期结束          */
} Mover_SeqTypeDef;

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief  建本模块的两条任务: rs485(总线) + mover(时序推进)。
 * @return SVC_OK(幂等) / SVC_ERR_HW(建任务失败)。
 * @note   没有 mover 任务时 0xD0..0xD6 会回 ERR —— 非阻塞发射必须有推进者。
 */
Svc_StatusTypeDef Mover_App_Start(void);

/**
 * @brief  挂上一条发射(只挂不跑, 立刻返回; 由任务逐帧推进)。
 * @param  Mmps : 发射速度(mm/s), 必须在档位范围内。
 * @return SVC_OK / SVC_ERR_STATE(上一条还在跑, 或任务没起来) / SVC_ERR_PARAM。
 */
Svc_StatusTypeDef Mover_App_Launch(int32_t Mmps);

/** @brief  当前时序状态。 */
Mover_SeqTypeDef Mover_App_GetState(void);

/** @brief  状态名(日志用)。 */
const char *Mover_App_StateName(Mover_SeqTypeDef State);

/**
 * @brief  处理 0xD0..0xD6(由 inter_app 的命令地图调用)。
 * @return 1 = 报文里有本模块的字节; 0 = 没有(放行)。
 */
int Mover_App_Command(void *Arg, const uint8_t *Data, uint16_t Len, Svc_Net_ReplyTypeDef *R);

#endif /* MOVER_APP_H */
