/**
 *******************************************************************************
 * @file    service_ads.h
 * @brief   ADS8688 8 通道 ADC 采集(独占 PS SPI0, Mode 0)
 *
 * 通道:  CH0..CH4 = 线圈 1..5   CH5 = 母线   CH6 = 激光   CH7 = 备用
 * 线圈 5 路报电流(A), 换算 I = (1.5*V - 2.5)*10; 母线按 11:1 还原成母线电压;
 * 激光走状态机: 有效区 380..2020 mV 折成位移(mm), 两拍都有效才算速度(mm/s),
 * 丢失目标时速度报 0、位移保持上一拍有效值。
 * 每拍一行: DATA <秒> <i1>..<i5> <激光速度mm/s> <母线电压V> <激光位移mm>
 * 采样开关由上位机 0xE0 / 0xE1 控制; 通道对照表是 service_ads.c 的 gAdsCfg[]。
 *******************************************************************************
 */

#ifndef SERVICE_ADS_H
#define SERVICE_ADS_H

#include <stdint.h>

#include "service/service.h"
#include "service/inter_module/service_net.h"     /* Svc_Net_ReplyTypeDef */

/* -------------------------------------------------------------------------- */
/*  组件参数                                                                   */
/* -------------------------------------------------------------------------- */

#define SVC_ADS_STACK_WORDS     640U    /**< 2.5 KB                          */
#define SVC_ADS_PRIO            3U      /**< 采集要稳                         */

/** 采样 + 广播周期(ms)。 */
#define SVC_ADS_PERIOD_MS       10U

/** 上电配置最多试几次(间隔 50 ms)。 */
#define SVC_ADS_INIT_TRIES      1U

/** 1 = 只跑初始化命令并读回寄存器, 之后不再访问总线(排障用)。 */
#define SVC_ADS_INIT_ONLY       0U

/** 1 = 上电直接采样; 0 = 等上位机 0xE0。 */
#define SVC_ADS_RUN_DEFAULT     0U

/** 1 = 单次扫描(每路先发 MAN_Ch_n 再读); 0 = 自动连续扫描。 */
#define SVC_ADS_MANUAL_SCAN_DEFAULT 1U

/** 器件通道数(= gAdsCfg[] 条目数)。 */
#define SVC_ADS_CHANNELS        8U

/** 线圈路数(CH0..CH4)。 */
#define SVC_ADS_COILS           5U

/**
 * 线圈电流零点: "零电流时这一路的引脚电压"(mV)。
 *   出厂 1667 mV(= I[A] = (1.5V−2.5)·10 的零点)。实际每一路都有几十 mV 的漂移,
 *   用 Svc_Ads_RequestZeroCalib()(网口 0xE3)在**制动关闭、线圈无电流**时采几次平均值装进去。
 */
#define SVC_ADS_COIL_ZERO_MV    1667

/** 零点死区(mA): 换算结果落在 ±这个范围就当作 0, 免得零点噪声让曲线一直抖。 */
#define SVC_ADS_COIL_DEADBAND_MA 30

/* -------------------------------------------------------------------------- */
/*  激光位移标定(CH6): 现场只需要改这一块                                       */
/* -------------------------------------------------------------------------- */
/*
 * 四点标定(厂家/现场两点实测): 位移 x1..x2 (mm) 对应电压 y1..y2 (V),
 * 中间按直线插值 —— 手册 4.5.2 给的也是这个式子(把 y 和 x 的位置换一下)。
 *
 *     x[mm] = x1 + (x2 - x1) / (y2 - y1) * (V[V] - y1)
 *
 * 现在这组数: 0.732 V -> 0 mm, 1.143 V -> 480 mm, 也就是 411 mV 跨 480 mm。
 *
 * ⚠ 分辨率提醒(算速度会受影响): 411 mV / 480 mm -> **1 mV ≈ 1.17 mm**;
 *   ADS 这一档一个 LSB ≈ 0.078 mV ≈ 0.09 mm, 传感器 DA 精度 ±0.05% F.S. 折过来
 *   还有 1 mm 量级。所以 10 ms 差分会带着 ~十 mm/s 的量化抖动。
 *   要更细就把传感器里"映射起点/终点"收到实际行程(0..480), 让 480 mm 铺满整个输出
 *   量程(4~20 mA 就是 1.6 V), 分辨率能好 4 倍左右。
 *
 * 电压下限/上限(判定用)还是按手册 4~20 mA + 100 Ω 取样电阻那组: 3.0/3.8/20.2/21.0 mA
 * x100 Ω = 300/380/2020/2100 mV —— 它是"读数有效/无效/超映射窗口"的硬界线, 和上面
 * 两点标定是两回事(标定只决定窗口内怎么换算)。
 */

/** 位移映射起点 / 终点(mm), 以及它们对应的引脚电压(V)。 */
#define SVC_ADS_LASER_X1_MM         0.0f
#define SVC_ADS_LASER_X2_MM         480.0f
#define SVC_ADS_LASER_Y1_V          0.732f
#define SVC_ADS_LASER_Y2_V          1.143f

/** 判定阈值(mV, = 手册里的 4~20 mA 余量值/错误值 x 100 Ω): 见文件下方状态表。 */
#define SVC_ADS_LASER_LOST_MV       300     /**< <  这个值 = 丢失(无效值 3.0 mA)    */
#define SVC_ADS_LASER_DEBOUNCE_MV   380     /**< 到 380 之间 = 过渡防抖区(余量 3.8 mA) */
#define SVC_ADS_LASER_TRACK_MAX_MV  2020    /**< 到这儿为止 = 有效跟踪区(余量 20.2 mA) */
#define SVC_ADS_LASER_LOST_MAX_MV   2100    /**< 超过这个值 = 丢失(无效值 21.0 mA)   */

/** 连续多少次满足才切换状态(抑制 ADC 噪声误触发, 1 = 不防抖)。 */
#define SVC_ADS_LASER_DEBOUNCE_N    3U

/** 测速用的采样周期(ms): v = Δx / dt, 和 SVC_ADS_PERIOD_MS 一致。 */
#define SVC_ADS_LASER_DT_MS         10U

/**
 * 速度要不要取绝对值: 1 = 报"速率"|Δx/Δt|(永远 >= 0), 0 = 保留符号。
 *
 * 默认 1 的原因: 位移是按映射起点/终点算的"位置读数", 动子往传感器靠近时它一路**减小**,
 * 于是 Δx/Δt 是负的 —— 曲线会变成一条向下的谷, 看着不像"先加速后减速"。取绝对值之后
 * 速度就是一条先升后降的正曲线, 正好对上制动过程的直觉。
 *
 * ⚠ 代价: 动子**反向**运动也会被折成正数, 光看速度曲线分不出方向(位移曲线仍然分得出)。
 *   要方向就把这里改成 0。
 */
#define SVC_ADS_LASER_SPEED_ABS     1

/*
 * ---- 平滑(一阶低通 IIR): 1.0 = 不滤波, 越小越平滑、滞后越大 ------------------
 *
 * 为什么要滤: 这套标定下 1 mV ≈ 1.17 mm, ADS 一个 LSB(0.078 mV)≈ 0.09 mm, 10 ms 直接
 * 差分就是 **±9 mm/s 的量化抖动**, 而且位移曲线会出现台阶。实测 100 Hz 采样下:
 *
 *     alpha   等效截止        噪声压到    滞后
 *     1.00    不滤            1x          0
 *     0.50    ~12 Hz          0.58x       ~1 拍
 *     0.35    ~7.6 Hz         0.44x       ~2 拍
 *     0.20    ~3.9 Hz         0.33x       ~4 拍
 *
 * 顺序是"先把位移滤平滑 -> 再用滤波后的位移差分算速度 -> 再对速度轻滤一次", 这样位移和
 * 速度仍然是同一套数(速度 ≈ 滤波后位移的导数), 不会各滤各的互相矛盾。
 * 现场觉得"太肉"(刹车瞬间被抹平)就把这两个值往大调; 想看原始数据就都改回 1.0。
 * eMMC 录的和 DATA 行发的是**同一份滤波后的值**, 存下来的和看到的一致。
 */
#define SVC_ADS_LASER_DISP_ALPHA    0.35f   /**< 位移低通系数(0..1)      */
#define SVC_ADS_LASER_SPEED_ALPHA   0.30f   /**< 速度低通系数(0..1)      */

/**
 * 速度死区(mm/s): 算出来的速率低于它就报 0。0 = 不要死区。
 *
 * 为什么必须有: 动子停住时位移只剩 ADC 那 ±1 个 LSB(≈0.09 mm)的抖动, 10 ms 一差分
 * 就是十几 mm/s; 而速度又取了绝对值 —— 零均值的噪声被"整流"成**永远为正**, 于是静止
 * 时曲线是一条贴在 0 上方的毛带, 看着像"车停了还在跑"。
 *
 * 调法: 静止时还剩毛刺就往大调(20 -> 40); 觉得低速段的曲线被抹掉了(比如刹车尾巴
 * 直接掉到 0)就往小调。制动试验的速度都是几百 mm/s, 20~40 mm/s 的死区不影响读数。
 */
#define SVC_ADS_LASER_SPEED_DEADBAND_MMS  20

/** 激光状态(快照里报出去, 也用来判"上一拍是否有效")。 */
#define SVC_ADS_LASER_STATE_LOST    0U      /**< 丢失目标: 不折位移、不算速度    */
#define SVC_ADS_LASER_STATE_TRACK   1U      /**< 有效跟踪: 折位移、两拍都有效才算速度 */

/** 零点标定默认采多少轮(10 ms 一轮 -> 200 轮 = 2 s)。 */
#define SVC_ADS_ZERO_ROUNDS     200U

/** 零点标定采样轮数上限(防止传个离谱的值把采集任务卡住)。 */
#define SVC_ADS_ZERO_ROUNDS_MAX 1000U

/** 序列使能掩码(bit0 = ch0), 决定采哪几路; 未使能的通道在 DATA 行里是 0。 */
#define SVC_ADS_SEQ_MASK        0xFFU

/** 遥测广播默认开关(可用 0xE0 / 0xE1 在线改)。 */
#define SVC_ADS_STREAM_DEFAULT  1U

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                     */
/* -------------------------------------------------------------------------- */

/** 一拍采集出来的样本(给录 eMMC 这类订阅者用)。 */
typedef struct {
    uint32_t UptimeMs;                          /**< 采样时刻(uptime 毫秒)      */
    int32_t  CoilMilliAmps[SVC_ADS_COILS];      /**< 线圈 1..5 的电流(mA)       */
    int32_t  VbusMilliVolts;                    /**< 母线电压(mV) = CH5 * 11     */
    int32_t  SpeedMilliMmS;                     /**< 激光测速(毫 mm/s); 丢失时 0 */
    int32_t  LaserMilliMm;                      /**< 激光位移(毫 mm); 丢失时保持上一有效值 */
    int32_t  LaserMilliVolts;                   /**< 激光引脚电压(mV); 排障看原始数据用 */
} Svc_Ads_SampleTypeDef;

/** 样本订阅回调: 在 ADS 任务里调, 实现必须"拷走就返回"。 */
typedef void (*Svc_Ads_SampleSinkTypeDef)(const Svc_Ads_SampleTypeDef *Sample);

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                     */
/* -------------------------------------------------------------------------- */

/** 初始化 PS SPI0(主机, Mode 0)。不下发器件配置、不碰中断。 */
Svc_StatusTypeDef Svc_Ads_Init(void);

/** 初始化好了没有(1 = 好了)。 */
uint8_t Svc_Ads_IsReady(void);

/** 采集任务入口(10 ms 扫一轮 + 广播 DATA); 任务由应用层创建。 */
void Svc_Ads_Task(void *Arg);

/** 订阅每一拍的样本; NULL = 取消。要在采集任务启动前挂上。 */
Svc_StatusTypeDef Svc_Ads_SetSampleSink(Svc_Ads_SampleSinkTypeDef Sink);

/** 开始/停止采样(上位机 0xE0 / 0xE1 落到这里)。On = 0 时不扫、不广播、不送样本。 */
Svc_StatusTypeDef Svc_Ads_SetRunning(int On);

/** 在不在采样(1 = 每 10 ms 一轮 + 广播)。 */
int Svc_Ads_IsRunning(void);

/** 广播开不开(用来区分"采了但没发")。 */
int Svc_Ads_IsStreaming(void);

/**
 * @brief  请求零点标定: 采 Rounds 轮"当前引脚电压"取平均, 装成以后每路的归零电压。
 * @param  Rounds : 轮数(10 ms 一轮); 0 = 用 SVC_ADS_ZERO_ROUNDS, 上限 SVC_ADS_ZERO_ROUNDS_MAX。
 * @note   **调用前必须保证制动关闭、线圈里没有电流**, 否则标定出来的零点就是错的。
 *         必须在采样中(0xE0 之后)调用; 标定期间数据照常出, 完成后会广播一行结果。
 * @return SVC_OK / SVC_ERR_STATE(没在采样)。
 */
Svc_StatusTypeDef Svc_Ads_RequestZeroCalib(uint32_t Rounds);

/** @brief  在不在零点标定中(1 = 正在累加)。 */
int Svc_Ads_IsZeroCalibrating(void);

/**
 * @brief  读当前每路的归零电压(mV)。
 * @param  Out   : 至少 SVC_ADS_COILS 个元素的数组。
 * @param  Count : 数组容量。
 */
Svc_StatusTypeDef Svc_Ads_GetZeroMv(int32_t *Out, uint32_t Count);

/**
 * @brief  读 5 路线圈电流(mA) —— 电流闭环的反馈量。
 * @param  Out   : 至少 SVC_ADS_COILS 个元素的数组。
 * @param  Count : 数组容量。
 */
Svc_StatusTypeDef Svc_Ads_GetCoilCurrentMa(int32_t *Out, uint32_t Count);

/**
 * @brief  读激光这一拍的结果(排障/上位机显示用)。
 * @param  State        : 输出 SVC_ADS_LASER_STATE_LOST / _TRACK; 可以为 NULL。
 * @param  MilliVolts   : 输出这一拍 CH6 的引脚电压(mV); 可以为 NULL。
 * @param  MilliMm      : 输出当前位移(毫 mm); 可以为 NULL。
 * @param  SpeedMilliMmS: 输出当前速度(毫 mm/s); 可以为 NULL。
 */
void Svc_Ads_GetLaserState(uint8_t *State, int32_t *MilliVolts,
                           int32_t *MilliMm, int32_t *SpeedMilliMmS);

#endif /* SERVICE_ADS_H */
