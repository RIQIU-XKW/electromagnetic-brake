/**
 *******************************************************************************
 * @file    service_motor.c
 * @brief   Mover_module 的电机组件实现 —— 只负责"把一帧 CiA402 动作发出总线"
 *
 * ⚠ 这个文件里**没有命令、没有时序、没有任务**(2026-10-16 拆分):
 *     原来它同时装着 发射状态机 + motor 任务 + MOTORLAUNCH 命令解析,
 *     现在那些全在 app/Mover_app.c(业务层), 本文件只剩"发一帧"的原子动作。
 *   所以改发射节奏(等多久、发哪几帧)去 Mover_app.c, 改寄存器/帧格式才来这里。
 *******************************************************************************
 */

#define LOG_ENABLE 1   /* 每帧打一行(寄存器/值/长度) —— 排查"电机不动/走错"就靠它;
                        * 嫌吵改成 0 即可(关掉只是少了日志, 行为不变)。 */
#define LOG_TAG "motor"
#include "service/monitor_module/log/log.h"

#include "service/Mover_module/service_motor.h"
#include "service/Mover_module/motor_cia402.h"
#include "service/Mover_module/modbus_rtu.h"

#include "service/Mover_module/service_rs485.h"    /* 本组件寄在 485 组件上 */

#include "osal/osal.h"

#include <string.h>

/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

static uint8_t gSlave = MOTOR_SLAVE_ADDR_DEFAULT;

/**
 * 上电初始化(0x6060=1 / 0x6040=6 / 0x6040=7)有没有成功下发过.
 *
 * ⚠ 只是个状态量: 发射链路**每次都重新初始化**(不看它), 见 app/Mover_app.c 的时序表。
 */
static uint8_t gInited;

/* ========================================================================== */
/*  底层: 发一帧(只发不收)                                                      */
/* ========================================================================== */

/**
 * @brief  把一帧发上 485 总线(**只发不收**).
 *
 * 2026-10-15 裁剪: 原来这里是"发一帧 + 等回复 + 收满 + 解析", 带总线锁、超时计数、
 * 回复快照。现在驱动器**只接收不回复**, 所以:
 *   - 只剩"拿总线锁 -> 发 -> 放锁", **不等待、不轮询、不解析**;
 *   - 发送成功(485 组件收下了)就返回 SVC_OK, 不再关心对方回不回;
 *   - 帧间那点间隔仍然保留(Modbus RTU 要求帧之间有空隙)。
 */
static Svc_StatusTypeDef Svc_Motor_Transact(const uint8_t *Frame, uint16_t Len)
{
    Svc_StatusTypeDef rc;

    /* 拿住总线: 发帧期间不让别的调用者插进来(半双工, DE 方向是共享的) */
    Svc_Rs485_BusLock();

    rc = Svc_Rs485_Send(Frame, Len);

    Svc_Rs485_BusUnlock();

    if (rc != SVC_OK) {
        return SVC_ERR_HW;
    }

    /* Modbus RTU 要求两帧之间有间隔 */
    Osal_DelayMs(SVC_MOTOR_FRAME_GAP_MS);

    return SVC_OK;
}

/** @brief  写一个 16 位寄存器(0x06). */
static int Svc_Motor_WriteU16(uint16_t Reg, uint16_t Value)
{
    uint8_t  frame[MB_FRAME_MAX];
    uint16_t len = Mb_BuildWriteSingle(frame, (uint16_t)sizeof(frame), gSlave, Reg, Value);

    if (len == 0U) {
        return 0;
    }

    /* ⚠ 逐帧日志(排障用): 驱动器只收不回, 出了"电机不动/走错"这类问题只能靠这一行
     *   确认"帧到底发没发出去、发的是什么"。寄存器 + 值 + 帧长三样都要有。 */
    LOG_I("tx 0x06 reg=0x%04X val=0x%04X len=%u\r\n",
          (unsigned)Reg, (unsigned)Value, (unsigned)len);

    return (Svc_Motor_Transact(frame, len) == SVC_OK) ? 1 : 0;
}

/** @brief  写一个 32 位对象(0x10 写 2 个寄存器, 高字在前). */
static int Svc_Motor_WriteU32(uint16_t Reg, uint32_t Value)
{
    uint8_t  frame[MB_FRAME_MAX];
    uint16_t len = Mb_BuildWriteU32(frame, (uint16_t)sizeof(frame), gSlave, Reg, Value);

    if (len == 0U) {
        return 0;
    }

    LOG_I("tx 0x10 reg=0x%04X val=0x%08X (%d) len=%u\r\n",
          (unsigned)Reg, (unsigned)Value, (int)Value, (unsigned)len);

    return (Svc_Motor_Transact(frame, len) == SVC_OK) ? 1 : 0;
}

/* ========================================================================== */
/*  CiA402 动作(组合命令复用)                                                  */
/* ========================================================================== */

int Svc_Motor_DoInit(void)
{
    int ok = 1;

    if (Svc_Motor_WriteU16(CIA402_OD_MODES_OF_OP, (uint16_t)CIA402_MODE_PROFILE_POSITION) == 0) {
        ok = 0;
    }
    if (Svc_Motor_WriteU16(CIA402_OD_CONTROLWORD, CIA402_CW_CMD_SHUTDOWN) == 0) {
        ok = 0;
    }
    if (Svc_Motor_WriteU16(CIA402_OD_CONTROLWORD, CIA402_CW_CMD_SWITCH_ON) == 0) {
        ok = 0;
    }

    if (ok != 0) {
        gInited = 1U;
    }

    return ok;
}

int Svc_Motor_DoEnable(void)
{
    return Svc_Motor_WriteU16(CIA402_OD_CONTROLWORD, CIA402_CW_CMD_ENABLE_OP);
}

int Svc_Motor_DoStop(void)
{
    /*
     * shutdown 之后驱动器回到 "Ready to switch on"; 标准 CiA402 里从这个状态要
     * **先 Switch on(0x0007) 再 0x000F** 才能到 Operation enabled, 单发 0x000F 不够。
     * 所以清掉"已初始化"标记: 下次发射会重新走一遍初始化那三条。
     */
    gInited = 0U;

    return Svc_Motor_WriteU16(CIA402_OD_CONTROLWORD, CIA402_CW_CMD_SHUTDOWN);
}

int Svc_Motor_DoSpeed(int32_t Mmps)
{
    const int32_t value = MOTOR_MMPS_TO_VELOCITY(Mmps);

    return Svc_Motor_WriteU32(CIA402_OD_PROFILE_VELOCITY, (uint32_t)value);
}

int Svc_Motor_DoPosition(int32_t Value)
{
    return Svc_Motor_WriteU32(CIA402_OD_TARGET_POSITION, (uint32_t)Value);
}

int Svc_Motor_DoGo(void)
{
    return Svc_Motor_WriteU16(CIA402_OD_CONTROLWORD, CIA402_CW_CMD_START_MOVE);
}

/* ========================================================================== */
/*  对外接口                                                                    */
/* ========================================================================== */

Svc_StatusTypeDef Svc_Motor_Init(void)
{
    gSlave  = MOTOR_SLAVE_ADDR_DEFAULT;
    gInited = 0U;

    LOG_I("ready: slave 0x%02X, frame gap %u ms (tx only)\r\n",
          (unsigned)gSlave, (unsigned)SVC_MOTOR_FRAME_GAP_MS);

    return SVC_OK;
}
