/**
 *******************************************************************************
 * @file    ps_spi.c
 * @brief   PS SPI0 主机 + CS/DAISY/RST(axi_gpio_1) —— 实现
 *
 * 骨架和 STM32 那版(实验67 ads8688.c)一样: 初始化 -> 收发一帧 -> 几个小工具。
 * 硬件连线、三条不能改的事、时钟表见 ps_spi.h。
 *******************************************************************************
 */

#include "bsp/ps_spi/ps_spi.h"

#include <string.h>

#include "xstatus.h"
#include "xspips_hw.h"          /* 寄存器级访问(自己写带超时的传输) */
#include "osal/osal.h"          /* Osal_UptimeMs: 算超时            */

#define LOG_TAG     "spi"
#define LOG_ENABLE  0
#include "service/monitor_module/log/log.h"

#define PS_SPI_REF_CLK_FALLBACK   200000000U    /* BSP 没给频率时的兜底 */
#define PS_SPI_PRESCALER_MAX      30U
#define PS_SPI_SPIN_SHORT         300U          /* ≈1 µs: CS 建立/保持  */
#define PS_SPI_SPIN_RST           3000U         /* ≈10 µs: RST 低脉冲   */

/* 只在本文件里用的小工具(原来对外暴露过, 随 service/spi 一起收回内部)。
 * 它们只出现在 LOG_* 参数里, 所以本模块日志关掉(LOG_ENABLE = 0)时连定义一起编掉,
 * 免得 -Wall 报 unused-function —— 把上面 LOG_ENABLE 改成 1 它们就回来了。 */
#if (LOG_ENABLE != 0)
static const char *PsSpi_StrError(int32_t code);
static uint32_t    PsSpi_GetCtrlReg(const PsSpi_HandleTypeDef *h);
#endif

/* Init 在它前面, 所以先声明(定义在文件末尾附近)。 */
static PsSpi_StatusTypeDef PsSpi_SetMode(PsSpi_HandleTypeDef *h, uint8_t Mode);

/** @brief  空转(volatile, 不会被优化掉). */
static void PsSpi_Spin(uint32_t loops)
{
    volatile uint32_t i;

    for (i = 0U; i < loops; i++) {
        __asm__ volatile ("nop");
    }
}

#if (LOG_ENABLE != 0)
static const char *PsSpi_StrError(int32_t code)
{
    switch (code) {
    case PS_SPI_OK:           return "ok";
    case PS_SPI_ERR_PARAM:    return "bad-param";
    case PS_SPI_ERR_INIT:     return "not-ready";
    case PS_SPI_ERR_BUSY:     return "busy";
    case PS_SPI_ERR_XFER:     return "xfer";
    case PS_SPI_ERR_TIMEOUT:  return "timeout";
    case PS_SPI_ERR_GPIO:     return "gpio";
    default:                  return "unknown";
    }
}

static uint32_t PsSpi_GetCtrlReg(const PsSpi_HandleTypeDef *h)
{
    return (h != NULL) ? XSpiPs_ReadReg(h->Spi.Config.BaseAddress, XSPIPS_CR_OFFSET) : 0U;
}
#endif /* LOG_ENABLE */

/** @brief  取 SCLK = ref/2^(N+1) 里最接近 target 的 N. */
uint8_t PsSpi_PrescalerFor(uint32_t refClkHz, uint32_t targetHz)
{
    uint8_t  n, best = 0U;
    uint32_t bestDiff = 0xFFFFFFFFU;

    if ((refClkHz == 0U) || (targetHz == 0U)) {
        return 0U;
    }
    for (n = 0U; n <= (uint8_t)PS_SPI_PRESCALER_MAX; n++) {
        const uint32_t sclk = refClkHz / ((uint32_t)1U << (n + 1U));

        if (sclk <= targetHz) {                 /* 到这一档就够了, 比一比就收 */
            const uint32_t diff = targetHz - sclk;

            if (diff < bestDiff) {
                best = n;
            }
            break;
        }
        bestDiff = sclk - targetHz;
        best = n;
    }
    return best;
}

/* ========================================================================== */
/*  三根控制脚: 一根一个对象 + 各自的接口                                        */
/* ========================================================================== */

/*
 * 引脚表 —— **认引脚只认这里**(和 base_system_pins.xdc 第 5 节的 set_property 一一对应):
 *
 *     CS    -> M18   gpio_io_o[0]  低有效   空闲高   每帧一个脉冲
 *     DAISY -> L14   gpio_io_o[1]  只能为低  空闲低   拉高进菊花链模式
 *     RST   -> L15   gpio_io_o[2]  低有效   空闲高   除上电那一次脉冲外不动
 *     bit3/4 -> L20 / M14          备用(没接线, 所以这里不再给它们建对象)
 *
 * 之所以把"引脚号"也放进对象: PsSpi_Init() 会把这张表打进上电日志, 于是
 * "软件认为 CS 在哪个脚"和"板子上 CS 接在哪个脚"可以直接对着日志核, 不用翻注释。
 */
const PsSpi_PinTypeDef PS_SPI_PIN_CS =
    { PS_SPI_GPIO_BASEADDR, PS_SPI_GPIO_CHANNEL, 0U, 0U, 1U, "CS",     "M18" };
const PsSpi_PinTypeDef PS_SPI_PIN_DAISY =
    { PS_SPI_GPIO_BASEADDR, PS_SPI_GPIO_CHANNEL, 1U, 0U, 0U, "DAISY",  "L14" };
const PsSpi_PinTypeDef PS_SPI_PIN_RST =
    { PS_SPI_GPIO_BASEADDR, PS_SPI_GPIO_CHANNEL, 2U, 0U, 1U, "RST",    "L15" };

/** @brief  把输出字影子整字写下去 —— **全文件只有这一处写 AXI GPIO 通道 1**. */
static PsSpi_StatusTypeDef PsSpi_WriteWord(PsSpi_HandleTypeDef *h)
{
    if ((h == NULL) || (h->GpioReady == 0U)) {
        return PS_SPI_ERR_GPIO;
    }

    XGpio_DiscreteWrite(&h->Gpio, (unsigned)PS_SPI_GPIO_CHANNEL, (u32)h->Ctrl);

    return PS_SPI_OK;
}

PsSpi_StatusTypeDef PsSpi_PinWrite(PsSpi_HandleTypeDef *h, const PsSpi_PinTypeDef *Pin,
                                   uint8_t Level)
{
    if ((h == NULL) || (h->GpioReady == 0U) || (Pin == NULL) || (Pin->Bit >= 5U)) {
        return PS_SPI_ERR_PARAM;
    }

    /*
     * 封装保护: **DAISY 只允许低**。拉高会器件进菊花链模式(命令写不进去、读回是数据流),
     * 一句误操作就能毁掉整条链路, 所以这里直接拒掉并留一行日志 —— 而不是"允许但注释提醒"。
     */
    if ((Pin == &PS_SPI_PIN_DAISY) && (Level != 0U)) {
        LOG_E("refused: driving DAISY (%s) high would put the ADC into daisy-chain mode\r\n",
              Pin->BoardPin);

        return PS_SPI_ERR_PARAM;
    }

    if (Level != 0U) {
        h->Ctrl |= (uint8_t)(1U << Pin->Bit);
    } else {
        h->Ctrl &= (uint8_t)~(1U << Pin->Bit);
    }

    return PsSpi_WriteWord(h);
}

uint8_t PsSpi_PinRead(const PsSpi_HandleTypeDef *h, const PsSpi_PinTypeDef *Pin)
{
    if ((h == NULL) || (Pin == NULL) || (Pin->Bit >= 5U)) {
        return 0U;
    }

    return (uint8_t)((h->Ctrl >> Pin->Bit) & 0x01U);
}

/* —— 三根脚各自的语义接口 —— */

PsSpi_StatusTypeDef PsSpi_CsSelect(PsSpi_HandleTypeDef *h)
{
    return PsSpi_PinWrite(h, &PS_SPI_PIN_CS, PS_SPI_PIN_CS.ActiveLevel);
}

PsSpi_StatusTypeDef PsSpi_CsRelease(PsSpi_HandleTypeDef *h)
{
    return PsSpi_PinWrite(h, &PS_SPI_PIN_CS, (uint8_t)(PS_SPI_PIN_CS.ActiveLevel ^ 1U));
}

PsSpi_StatusTypeDef PsSpi_DaisyHold(PsSpi_HandleTypeDef *h)
{
    return PsSpi_PinWrite(h, &PS_SPI_PIN_DAISY, PS_SPI_PIN_DAISY.IdleLevel);   /* 恒为低 */
}

PsSpi_StatusTypeDef PsSpi_RstAssert(PsSpi_HandleTypeDef *h)
{
    return PsSpi_PinWrite(h, &PS_SPI_PIN_RST, PS_SPI_PIN_RST.ActiveLevel);     /* 低 = 复位 */
}

PsSpi_StatusTypeDef PsSpi_RstRelease(PsSpi_HandleTypeDef *h)
{
    return PsSpi_PinWrite(h, &PS_SPI_PIN_RST, PS_SPI_PIN_RST.IdleLevel);       /* 高 = 工作 */
}

PsSpi_StatusTypeDef PsSpi_RstPulse(PsSpi_HandleTypeDef *h, uint32_t LowUs)
{
    uint32_t loops;

    if (LowUs == 0U) {
        LowUs = PS_SPI_RST_LOW_US;
    }

    if (PsSpi_RstAssert(h) != PS_SPI_OK) {
        return PS_SPI_ERR_GPIO;
    }

    /* PsSpi_Spin(300) ≈ 1 µs(见文件头), 所以按微秒换算成空转圈数 */
    loops = LowUs * (PS_SPI_SPIN_RST / (uint32_t)PS_SPI_RST_LOW_US);
    PsSpi_Spin(loops);

    return PsSpi_RstRelease(h);
}

PsSpi_StatusTypeDef PsSpi_CtrlIdle(PsSpi_HandleTypeDef *h)
{
    uint8_t word;

    if ((h == NULL) || (h->GpioReady == 0U)) {
        return PS_SPI_ERR_GPIO;
    }

    /* 按三根脚**各自的空闲电平**拼出整字, 一次写下去(中间没有"半空闲"的瞬态)。
     * bit3/4(L20/M14)没接线, 表里也没有它们的对象 —— 整字里保持 0(上电默认)即可。 */
    word  = (uint8_t)(PS_SPI_PIN_CS.IdleLevel    << PS_SPI_PIN_CS.Bit);
    word |= (uint8_t)(PS_SPI_PIN_DAISY.IdleLevel << PS_SPI_PIN_DAISY.Bit);
    word |= (uint8_t)(PS_SPI_PIN_RST.IdleLevel   << PS_SPI_PIN_RST.Bit);

    h->Ctrl = word;

    if (word != (uint8_t)PS_SPI_CTRL_IDLE) {
        LOG_W("idle word 0x%02X != PS_SPI_CTRL_IDLE 0x%02X (pin table changed?)\r\n",
              (unsigned)word, (unsigned)PS_SPI_CTRL_IDLE);
    }

    return PsSpi_WriteWord(h);
}

uint8_t PsSpi_GetCtrlCode(const PsSpi_HandleTypeDef *h)
{
    return (h != NULL) ? (uint8_t)(h->Ctrl & 0x1FU) : 0U;   /* 5 位输出全给出来 */
}

/**
 * @brief  回读: bit0..4 = AXI GPIO 通道1(输出字)寄存器读回, bit5 = 通道2 输入脚(L19).
 * @note   通道1 那 5 位是**输出**, 读回来的是"写下去的值"(GPIO_DATA 寄存器), 所以它只能
 *         证明"软件/总线/IP 这一侧对了"; 引脚在板子上到底是什么电平, 得靠一根跳线把
 *         M18/L14/L15 接到空闲输入脚 L19, 再看 bit5 —— 那才是真正的线检。
 */
uint8_t PsSpi_ReadPins(PsSpi_HandleTypeDef *h)
{
    uint32_t out, in;

    if ((h == NULL) || (h->GpioReady == 0U)) {
        return 0U;
    }
    out = XGpio_DiscreteRead(&h->Gpio, (unsigned)PS_SPI_GPIO_CHANNEL);
    in  = XGpio_DiscreteRead(&h->Gpio, (unsigned)PS_SPI_GPIO2_CHANNEL);

    return (uint8_t)((out & 0x1FU) | ((in & 0x01U) << 5));
}

/**
 * @brief  打开 AXI GPIO: 通道1 输出(CS/DAISY/RST/备用), 通道2 输入, 写空闲电平.
 * @note   SDT 流程下 XGpio_LookupConfig() 收的是**基地址**: 写成 (u16) 会把
 *         0x41210000 截成 0, 而"地址 0"会匹配到第一个 GPIO(axi_gpio_0)。
 */
static PsSpi_StatusTypeDef PsSpi_GpioInit(PsSpi_HandleTypeDef *h)
{
    XGpio_Config *cfg = XGpio_LookupConfig((UINTPTR)PS_SPI_GPIO_BASEADDR);

    if ((cfg == NULL) ||
        (XGpio_CfgInitialize(&h->Gpio, cfg, cfg->BaseAddress) != XST_SUCCESS)) {
        LOG_E("AXI GPIO 0x%08X not found (平台重建了吗?)\r\n",
              (unsigned)PS_SPI_GPIO_BASEADDR);
        return PS_SPI_ERR_GPIO;
    }

    XGpio_SetDataDirection(&h->Gpio, (unsigned)PS_SPI_GPIO_CHANNEL, 0x00000000U);
    XGpio_SetDataDirection(&h->Gpio, (unsigned)PS_SPI_GPIO2_CHANNEL, 0x00000001U);
    h->GpioReady = 1U;

    /* 三根脚各自回空闲: CS 高(不选中) + DAISY 低(不进菊花链) + RST 高(不复位) */
    (void)PsSpi_CtrlIdle(h);

    return PS_SPI_OK;
}

PsSpi_StatusTypeDef PsSpi_Init(PsSpi_HandleTypeDef *h, uint32_t baseAddr)
{
    XSpiPs_Config *cfg;
    s32            st;

    if (h == NULL) {
        return PS_SPI_ERR_PARAM;
    }
    memset(h, 0, sizeof(*h));

    if (PsSpi_GpioInit(h) != PS_SPI_OK) {                   /* CS/DAISY/RST 先到位 */
        return PS_SPI_ERR_GPIO;
    }
    cfg = XSpiPs_LookupConfig(baseAddr);
    if ((cfg == NULL) ||
        (XSpiPs_CfgInitialize(&h->Spi, cfg, cfg->BaseAddress) != (s32)XST_SUCCESS)) {
        LOG_E("SPI0 0x%08X init failed\r\n", (unsigned)baseAddr);
        return PS_SPI_ERR_INIT;
    }
    h->RefClkHz = (cfg->InputClockHz != 0U) ? cfg->InputClockHz
                                           : (uint32_t)PS_SPI_REF_CLK_FALLBACK;

    /*
     * ⚠ 自检必须在**所有配置之前**: XSpiPs_SelfTest() 首尾各调一次 XSpiPs_Reset()
     *   (驱动文档: "Upon successful return from the self-test, the device is reset"),
     *   放后面会把主机位/分频/片选全擦掉 —— 现象是 CR 回到复位值 0x00020000(不是主机)、
     *   字节压进 TXD 也不移位, 传输一路 timeout。
     */
    st = XSpiPs_SelfTest(&h->Spi);
    if (st != (s32)XST_SUCCESS) {
        LOG_W("self test failed (%d)\r\n", (int)st);
    }

    /*
     * 配置顺序: 主机 -> 分频 -> 选 SS -> 使能。
     * 注意 XSpiPs_Enable() 是个宏, 它写的是 ER 寄存器, **不碰 CR**; CR 里的 MSTREN
     * 只能由 SetOptions(MASTER_OPTION) 设, 所以这两步都不能省。
     */
    (void)PsSpi_SetMode(h, 0U);                              /* Mode 0: CPOL=0/CPHA=0 */
    h->Prescaler = PsSpi_PrescalerFor(h->RefClkHz, (uint32_t)PS_SPI_TARGET_HZ);
    (void)XSpiPs_SetClkPrescaler(&h->Spi, h->Prescaler);
    h->SclkHz = h->RefClkHz / ((uint32_t)1U << (h->Prescaler + 1U));

    /*
     * ⚠ 这一句**不能省**(踩过: 现象就是 "transfer failed: timeout"):
     *   控制器复位后 CR[SSCTRL] = 0xF = "不选任何从机"(见驱动的 XSpiPs_ResetHw),
     *   而主机模式下**没有选中任何 SS 就不会移位** —— 字节压进 TX FIFO 后 TXOW 永远不来。
     *   板上 SS 引脚一根都没引出来, 但这里选的是"控制器内部用哪一路片选", 与引脚无关;
     *   厂商文档原话: "As a master, the SetSlaveSelect function must be called prior to
     *   this function." -> 选 SS0, SSCTRL 变成 0xE。
     */
    (void)XSpiPs_SetSlaveSelect(&h->Spi, (u8)PS_SPI_CS_INDEX);

    XSpiPs_Enable(&h->Spi);                     /* 写 ER: 允许主机事务 */

    h->Ready   = 1U;
    h->LastErr = (int32_t)PS_SPI_OK;

    /* 上电日志把"软件认为的三根脚"直接说出来, 现场可以对着接线核 */
    LOG_I("pins: CS=%s(bit%u) DAISY=%s(bit%u) RST=%s(bit%u) on %s ch%u, idle word 0x%02X\r\n",
          PS_SPI_PIN_CS.BoardPin,    (unsigned)PS_SPI_PIN_CS.Bit,
          PS_SPI_PIN_DAISY.BoardPin, (unsigned)PS_SPI_PIN_DAISY.Bit,
          PS_SPI_PIN_RST.BoardPin,   (unsigned)PS_SPI_PIN_RST.Bit,
          "axi_gpio_1", (unsigned)PS_SPI_GPIO_CHANNEL, (unsigned)PS_SPI_CTRL_IDLE);
    LOG_I("ready: ref %u Hz sclk %u Hz (N=%u) cr 0x%08X ctrl 0x%02X @axi_gpio_1 0x%08X\r\n",
          (unsigned)h->RefClkHz, (unsigned)h->SclkHz, (unsigned)h->Prescaler,
          (unsigned)PsSpi_GetCtrlReg(h), (unsigned)h->Ctrl,
          (unsigned)PS_SPI_GPIO_BASEADDR);

    /* RST 硬复位脉冲: 一直被拉着的话器件停在复位, 读回全 0 */
    (void)PsSpi_RstPulse(h, PS_SPI_RST_LOW_US);

    /*
     * ⚠ 这里**不要再加 XSpiPs_SetOptions()/SetMode()** —— 实机验证过: 在 Init 末尾多配
     *   一次控制器, 会把上面那套"主机 + 分频 + 片选"白配一遍(现象: AUTO_SEQ_EN 回读变回
     *   0xFF、**cfg=0**、寄存器写全部不落地)。
     *   Init 里保持 SetMode -> SetClkPrescaler -> SetSlaveSelect -> Enable 这个顺序就够了;
     *   SCLK 就此定型(在线改档位那条路 - 原来的 ADSCLK - 已经删了, 要改就改 ps_spi.h 的
     *   PS_SPI_TARGET_HZ 重新烧一次)。
     */

    return PS_SPI_OK;
}

/**
 * @brief  寄存器级收发, 带超时.
 * @note   不用 XSpiPs_PolledTransfer(): 它死等 SR[TXOW] 且没有超时, SPI 时钟不起来
 *         时会把调用它的任务永远卡住。片选由 PsSpi_Transfer 用 GPIO 包在外面。
 *         (SPI0_SS_I 在 BD 里接的是 1'b0, 所以顺手把 mode fault 也当错误报出来。)
 */
static PsSpi_StatusTypeDef PsSpi_Xfer(PsSpi_HandleTypeDef *h, const uint8_t *tx,
                                      uint8_t *rx, uint16_t len)
{
    const uint32_t base = h->Spi.Config.BaseAddress;
    const uint32_t deadline = Osal_UptimeMs() + (uint32_t)PS_SPI_XFER_TIMEOUT_MS;
    uint16_t i;

    XSpiPs_Enable(&h->Spi);

    for (i = 0U; i < len; i++) {                /* 整帧一次压进 FIFO */
        XSpiPs_WriteReg(base, XSPIPS_TXD_OFFSET, (uint32_t)tx[i]);
    }
    while ((XSpiPs_ReadReg(base, XSPIPS_SR_OFFSET) & XSPIPS_IXR_TXOW_MASK) == 0U) {
        if ((XSpiPs_ReadReg(base, XSPIPS_SR_OFFSET) & XSPIPS_IXR_MODF_MASK) != 0U) {
            XSpiPs_WriteReg(base, XSPIPS_SR_OFFSET, XSPIPS_IXR_MODF_MASK);
            h->LastErr = (int32_t)PS_SPI_ERR_XFER;
            return PS_SPI_ERR_XFER;             /* mode fault: SS_I=1'b0 + 手动片选 */
        }
        if ((int32_t)(Osal_UptimeMs() - deadline) >= 0) {
            h->LastErr = (int32_t)PS_SPI_ERR_TIMEOUT;
            return PS_SPI_ERR_TIMEOUT;          /* 时钟没动 */
        }
    }
    for (i = 0U; i < len; i++) {
        while ((XSpiPs_ReadReg(base, XSPIPS_SR_OFFSET) & XSPIPS_IXR_RXNEMPTY_MASK) == 0U) {
            if ((int32_t)(Osal_UptimeMs() - deadline) >= 0) {
                h->LastErr = (int32_t)PS_SPI_ERR_TIMEOUT;
                return PS_SPI_ERR_TIMEOUT;
            }
        }
        if (rx != NULL) {
            rx[i] = (uint8_t)XSpiPs_ReadReg(base, XSPIPS_RXD_OFFSET);
        } else {
            (void)XSpiPs_ReadReg(base, XSPIPS_RXD_OFFSET);
        }
    }
    return PS_SPI_OK;
}

PsSpi_StatusTypeDef PsSpi_Transfer(PsSpi_HandleTypeDef *h, const uint8_t *tx,
                                   uint8_t *rx, uint16_t len)
{
    PsSpi_StatusTypeDef rc;

    if ((h == NULL) || (tx == NULL) || (len == 0U) || (h->Ready == 0U)) {
        return PS_SPI_ERR_PARAM;
    }
    if (h->Busy != 0U) {                        /* 半帧保护 */
        return PS_SPI_ERR_BUSY;
    }
    h->Busy = 1U;

    if (PsSpi_CsSelect(h) != PS_SPI_OK) {
        h->Busy = 0U;
        return PS_SPI_ERR_GPIO;
    }
    PsSpi_Spin(PS_SPI_SPIN_SHORT);              /* CS 建立时间 */
    rc = PsSpi_Xfer(h, tx, rx, len);
    PsSpi_Spin(PS_SPI_SPIN_SHORT);              /* CS 保持时间 */
    (void)PsSpi_CsRelease(h);                   /* 出错也要抬 CS */
    h->Busy = 0U;

    if (rc != PS_SPI_OK) {
        h->Errors++;
        h->LastErr = (int32_t)rc;
        XSpiPs_Abort(&h->Spi);                  /* 清掉可能残留的半帧 */
#if (LOG_ENABLE != 0)
        {
            /* 把 SR/CR 一起打出来: 出问题时空口猜寄存器状态最费时间 */
            const uint32_t sr = XSpiPs_ReadReg(h->Spi.Config.BaseAddress, XSPIPS_SR_OFFSET);

            LOG_E("transfer failed (%u bytes): %s sr 0x%08X cr 0x%08X\r\n",
                  (unsigned)len, PsSpi_StrError(rc), (unsigned)sr,
                  (unsigned)PsSpi_GetCtrlReg(h));
        }
#endif
        return rc;
    }

    h->Transfers++;
    h->TxBytes += (uint32_t)len;
    h->RxBytes += (rx != NULL) ? (uint32_t)len : 0U;
    h->LastErr = (int32_t)PS_SPI_OK;

    return PS_SPI_OK;
}

/**
 * @brief  改 SPI 模式: 0..3, CPOL = Mode>>1, CPHA = Mode&1.
 *
 * 从机的采样沿是唯一没法从网表证明的东西, 而模式错了症状极隐蔽: 全 0 的 NO_OP 帧在任何
 * 位移下还是全 0(所以"读数据"看着正常), 但写命令字/读地址字错开一位就全变了 ——
 * 于是寄存器写永远不落地。
 *
 * @note   现在**只有 PsSpi_Init() 调它一次(Mode 0)** —— ADS8688 本来就只要 Mode 0;
 *         原来那套"让服务层逐个模式试探"的调试链随 service/spi 一起删了, 所以这个函数
 *         收回文件内部(改回对外的成本就是把它和头文件里那行声明加回来)。
 */
static PsSpi_StatusTypeDef PsSpi_SetMode(PsSpi_HandleTypeDef *h, uint8_t Mode)
{
    u32 options = (u32)XSPIPS_MASTER_OPTION;

    if ((h == NULL) || (Mode > 3U)) {
        return PS_SPI_ERR_PARAM;
    }

    if ((Mode & 0x02U) != 0U) {
        options |= (u32)XSPIPS_CLK_ACTIVE_LOW_OPTION;   /* CPOL = 1 */
    }
    if ((Mode & 0x01U) != 0U) {
        options |= (u32)XSPIPS_CLK_PHASE_1_OPTION;      /* CPHA = 1 */
    }

    /*
     * SetOptions 会把没点到的模式位清掉(CPOL/CPHA 变化时驱动内部还会 Disable/Enable
     * 一次), 所以这一句同时完成了"设新模式"和"清旧模式"。
     */
    if (XSpiPs_SetOptions(&h->Spi, options) != (s32)XST_SUCCESS) {
        return PS_SPI_ERR_INIT;
    }

    XSpiPs_Enable(&h->Spi);

    return PS_SPI_OK;
}

