===============================================================================
  app923 — Zynq-7000 + FreeRTOS:网口 / ADC 采集 / eMMC 录制 / 制动功率级 / 发射小车
===============================================================================

硬件    : Xilinx Zynq-7000 (XC7Z020, base_system)
            GEM0  MIO 16..27 + RGMII PHY          (PS 网口, 见"网口与协议")
            breath_led   0x43C00000  led[3:0] = K16 J16 K14 J14     (PL 呼吸灯 4 路)
            pwm_axi_ip   0x43C10000  5 个 H 桥 / 20 路 / 过流输入   (制动功率级)
            AXI UARTLite 0x42C00000  tx = D19, rx = D20             (RS-485, 直线电机)
            AXI GPIO     0x41200000  de_485[0] = G14                (RS-485 方向)
            PS EMIO GPIO 6 位        LS_I[0..5] = W15 V12 W13 V15 R19 P19 (光幕)
            PS SPI0      4 线        SCLK=N15 MISO=N16 MOSI=M17      (ADS8688)
            axi_gpio_1   0x41210000  CS=M18 DAISY=L14 RST=L15        (ADS 控制脚)
            SD1          MIO 46..51  eMMC, FatFs 盘 1:/data.csv
            PS MIO 按键  KEY1=MIO10(E9), KEY2=MIO11(C6)
工具链  : Vitis 2024.2 Unified IDE, 域 freertos_ps7_cortexa9_0 (SDT 流程)
协议栈  : lwip220, NO_SYS = 1, RAW API, DHCP
系统    : FreeRTOS, tick 1000 Hz, heap 64 KB, 10 个任务(见任务表), RTOS 被 osal/ 挡住
对端    : 上位机 = qt/Electromagnetic_braking(Qt6); 直线电机驱动器 = Modbus RTU 从站

===============================================================================
  这个板子做什么
===============================================================================

  PC ──TCP(:7)──> [net] ──命令分发──> 各 app 文件(制动 / 发射 / 采集 / 灯 / 按键)
                    │
                    ├─ 直接读写寄存器 ──> PL 功率级 pwm_axi_ip
                    ├─ 队列 ──> [led] ──> PL 呼吸灯
                    └─ 广播 ──> 所有客户端(回话文本 / DATA 行 / [0xB0][0xC0][0xAA])

  [ads]  10 ms 采一轮 ADS8688 ──> DATA 行(100 Hz) + 样本交给 [emmc]
                                    └─ 队列 ──> [emmc] ──> SD1:/data.csv
  [ls]   5 ms 采 6 路光幕 ──> 变化时广播 [0xB0][掩码]
  [brake] 5 ms 推进制动时序(按光幕放行 + 过流立刻全关)
  [mover] 按时刻推进发射小车整周期(RS-485 上发 Modbus RTU 帧)
  [mon]  1 s 打一行状态日志 + 每秒巡检过流
  [key]  20 ms 轮询两个按键(KEY1 软复位 / KEY2 重启 TCP 服务端)
  [net]  独占 lwIP, 收发 + 命令分发 + 广播队列

  分层(硬性规则, tools\check_invariants.ps1 逐条检查):

    main.c    Log_Init() -> App_Init() -> App_StartTasks() -> Osal_StartScheduler()
      |
    app/      业务层: 命令(各占一个字节段) + 时序状态机 + 建任务 + STATUS
      |       只调 service 的接口, 不许 include bsp/
    service/  组件层: 能力(采样/功率级/光幕/485/网口/灯/日志/按键), 不含命令
      |       不许 include app/
    bsp/      驱动层: 全部硬件接口, 不许 include app/ 或 service/
    osal/     系统抽象: 只有 osal_freertos.c 能出现 FreeRTOS 符号

    log(service/monitor_module/log)是跨层公共设施: BSP 也要打日志, 放 service 会造成
    BSP 反向依赖 Service。

  命名约定(每个功能一对):
    service/brake_module/  <-> app/brake_ctl_app.c    制动功率级 / 占空比 + 死区
    service/Algorithm_module/<-> app/current_app.c    电流负反馈闭环(PID)
    service/Samp_module/   <-> app/samp_app.c         ADS8688 采集 + eMMC 录制
    service/Mover_module/  <-> app/Mover_app.c        RS-485 + 直线电机 + 发射时序
    service/inter_module/  <-> app/inter_app.c        网口 / 命令地图 / 按键 / STATUS
    service/monitor_module/<-> app/monitor_app.c      灯 / 日志 / 周期巡检

===============================================================================
  十一个任务(全部在 app 层的 *App_Start() 里创建)
===============================================================================

  任务    创建处                     优先级 栈      周期 / 唤醒        职责
  ------  -------------------------  ------  ------  -----------------  ------------------------
  net     Inter_App_Start()             4    6 KB    每 2 ms            独占 lwIP, 收发 + 命令分发
  key     Inter_App_Start()             2    1.5 KB  每 20 ms           按键消抖 + 复位/重连动作
  led     Monitor_App_Start()           2    1.5 KB  队列阻塞           4 路呼吸灯
  mon     Monitor_App_Start()           1    2 KB    每 1 s             状态日志 + 过流巡检
  ls      BrakeCtl_App_Start()          2    1.5 KB  每 5 ms            光幕采样 + 消抖 + 上报
  brkctl  BrakeCtl_App_Start()          3    2 KB    每 5 ms            制动: 占空比/死区 + 过流巡检
  rs485   Mover_App_Start()             3    4 KB    起端口后睡          起端口(只在任务里 Start 一次)
  mover   Mover_App_Start()             2    2 KB    时序等待           发射整周期推进
  ads     Samp_App_Start()              3    2.5 KB  每 10 ms           独占 PS SPI0 读 ADS8688
  emmc    Samp_App_Start()              2    6 KB    队列 + 1 s 刷盘    CSV 落盘
  curloop Current_App_Start()           3    2 KB    每 10 ms           5 路独立 PID 电流闭环

  三条队列就是全部任务间通信:
    QNetTx  待广播给所有客户端的文本(归 net)
    QLed    待执行的灯命令(归 led)
    QEmmc   待落盘的样本(归 emmc, 深 128 条 ≈ 1.3 s 余量)

  跨任务共享的只有"只读快照 + 单调计数器"(32 位对齐读写是原子的)。

  参数分两层: 组件固有参数(周期/栈/优先级/队列深度)在组件自己的 .h;
  应用策略(DHCP/端口/监视周期/过流自动确认/各组件开关 APP_ENABLE_*)在 app/app_config.h。

===============================================================================
  文件清单
===============================================================================

  src/
    main.c                     入口(日志 -> 装配 -> 建任务 -> 调度器)
    UserConfig.cmake           源文件清单、头文件路径、编译选项(加文件记得同步)
    lscript.ld                 链接脚本

    app/                       ★业务层(命令 + 时序 + 装配)
      app.c / app.h            App_Init / App_StartTasks / 上电自检 / 任务汇总日志
      app_config.h             全部应用策略(只看这一个文件就能改行为)
      inter_app.c / .h         命令地图 + STATUS + 按键(0xAA..0xAC)
      brake_app.c / .h         旧的光幕制动时序状态机(不参与编译, 见文件末尾)
      brake_ctl_app.c / .h     制动控制: 五桥同步 + 占空比/死区帧(0xA0..0xA5) + 上报(0xB0/0xC0)
      current_app.c / .h       电流闭环: 0xBA 帧解析 + curloop 任务创建
      Mover_app.c / .h         发射整周期 + 命令(0xD0..0xD6)
      samp_app.c / .h          采集命令(0xE0..0xE3)
      monitor_app.c / .h       灯命令(0xF0..0xFF)

    service/                   ★组件层(只提供能力, 没有命令)
      service.h                Svc_StatusTypeDef
      Algorithm_module/        电流闭环算法(纯算法, 不碰寄存器)
        service_pid.h / .c          PID: 浮点积分 + 条件积分抗饱和 + 输出爬坡
        service_current_loop.h/.c   5 套独立 PID + 10 ms 控制任务 + 互锁
      inter_module/            网口 + 命令表 + 按键
        service_net.h / .c       net 任务、广播队列、快照、客户端钩子
        net_cmd.h / .c           注册表 + 分发(精确名 -> 兜底项 -> unknown)
        net_text.h / .c          不用 sprintf 的文本拼接
        service_key.h / .c       按键消抖 + 上报回调
      brake_module/            功率级 + 光幕 + 过流
        service_pwm.h / .c       5 桥 / 频率 / 死区 / 占空比 / 全局调制 / 状态行
        service_ls.h / .c        光幕 6 路采样 + 消抖 + 变化上报
        service_oc.h / .c        过流状态 + 事件位 + acknowledge
      Samp_module/             ADC + 存储
        ads8688.h / .c           ADS8688 协议层(纯逻辑, 可 PC 单测)
        service_ads.h / .c       10 ms 扫描 + 换算 + DATA 广播 + 样本订阅
        service_emmc.h / .c      样本队列 + CSV 组行 + 512 B 暂存 + 1 s f_sync
      Mover_module/            RS-485 + 直线电机
        service_rs485.h / .c     半双工发送 + 递归总线锁
        modbus_rtu.h / .c        Modbus RTU 组帧 + CRC16(纯逻辑)
        motor_cia402.h           CiA402 寄存器/控制字/单位
        service_motor.h / .c     发帧动作(初始化/使能/速度/位置/启动/停机)
      monitor_module/
        service_monitor.h / .c   周期日志 + 过流巡检
        led/service_led.h / .c   队列 + 4 路掩码 + 共享频率
        log/log.h / .c           跨层日志(整行加锁)

    osal/                      ★系统抽象
      osal.h                   任务/队列/互斥量/时间/中断(不含任何 FreeRTOS 类型)
      osal_freertos.c          唯一能 include FreeRTOS 的文件

    bsp/                       ★驱动层
      enet/                    GEM0 + lwIP(状态机/PHY 时钟/netif/TCP 服务)
      pwm/pwm.h .c             pwm_axi_ip:桥/占空比/死区/相位/过流
      led/led.h .c             breath_led:4 路使能 + 共享频率
      ls/ls.h .c               光幕:EMIO GPIO 读 + 消抖
      oc/bsp_oc.h .c           过流:读 PWM IP 的 STATUS
      rs485/                   UARTLite + DE + FIFO + HAL
      ps_spi/ps_spi.h .c       PS SPI0 主机 + CS/DAISY/RST 三根脚
      emmc/emmc.h .c           FatFs 封装(挂载/格式化/追加/整行回读)
      ps_key/ps_key.h .c       MIO 按键读电平
      ps_rst/ps_rst.h .c       PS 软复位

    _legacy_bringup/           早期 Xilinx lwip_echo_server(不参与编译)

===============================================================================
  网口与协议
===============================================================================

  静态地址 192.168.1.10(DHCP 时看串口打印), TCP 端口 7, RAW API —— 所有 lwIP 调用
  都在 net 任务里。

  ---- 下发: 文本命令(ASCII, 尾部 \r\n 去掉后整行比较) --------------------

    PING                回 pong
    STATUS              回一行总状态:
                        zynq | led=0x01/normal/100Hz | ip ... | net=up/100M/cli=1 |
                        pwm en=0x00000003 oc=0/0 | cmds=7 | ls=0x00 | ocEv=.. ocLt=.. |
                        ocNw=.. rpt=1
    其它                回 "unknown (N bytes); try ..."

  ---- 下发: 单字节命令(在整条报文里扫这些字节, 命中就执行) -----------------

  0xA0  制动使能(开全局调制, 桥仍关着)
  0xA1  启动制动(五路 H 桥同时打开, 用当前占空比; 五路同步)
  0xA2  预设占空比 30%      0xA3 50%      0xA4 70%(= 上限)
  0xA5  停止制动(五路全关 + 关全局调制)
  0xDA <hi> <lo> 0xDB  设占空比(0..70 %), 五路同步生效 —— 4 字节帧, 见下面"帧命令"
  0xDC <hi> <lo> 0xDD  设死区(0 或 100..10000 ns), 五路同步生效
  0xBA <lo> <hi> 0xAB  电流指令(0..15000 mA), 5 桥共用, 走电流闭环
  0xD0  用当前档位发射小车
  0xD1..0xD6  选 1..6 档(并记住)后立刻发射
  0xE0  开始采样(每 10 ms 扫一轮 + 广播 DATA)
  0xE1  停止采样
  0xE2  查采样状态(回一行文本)
  0xE3  电流零点标定(采当前引脚电压当零电流电压; 要求采样中 + 制动关闭)
  0xF0  四路全灭
  0xF1  慢呼吸(~5 s)      0xF2 标准(~1 s)      0xF3 快(~100 ms)
  0xF4  查灯状态          0xF5 四路全亮
  0xF6..0xF9  点亮 LED0..LED3        0xFA..0xFD  熄灭 LED0..LED3
  0xFE <n>    设共享频率, n = Hz/10(1..100 -> 10..1000 Hz)
  0xFF <m>    设四路使能掩码, m = 0..15
  0xAA  查按键状态(回一行文本)
  0xAB  软复位 PS(等于按 KEY1; 回复多半发不出去, 属正常)
  0xAC  重启 TCP 服务端(等于按 KEY2)

  参数规则: 0xFE / 0xFF 吃掉**同一报文**的下一个字节; 参数没跟上回一句用法提示。

  ---- 下发: 帧命令(4 字节, 必须同一次写入) --------------------------------

    [0xDA][占空比高字节][占空比低字节][0xDB]   占空比 0..70(%) —— 五路桥同步
    [0xDC][死区高字节][死区低字节][0xDD]       死区 0 或 100..10000(ns), 20 ns 一格
    [0xBA][电流低字节][电流高字节][0xAB]       电流指令 0..15000(mA) —— 5 桥共用,**低字节在前**

  高字节在前(大端)。帧头/帧尾任一不对就回一句用法提示, 不会误动作:
    帧头对但第 4 字节不是帧尾 -> "bad frame tail (0xDA needs 0xDB, 0xDC needs 0xDD)"
    帧被拆到下一个报文      -> "frame truncated (need head + 16bit + tail)"
    数值越界                -> "duty must be 0..70 (%)"
                               "dead time must be 0 (duty 0) or >= 100 ns"
    电流帧帧尾不对          -> "current ERR: frame must be [0xBA][lo][hi][0xAB]"

  ⚠ 电流帧是**低字节在前**(按现场给的 `{0xBA, 低字节, 高字节, 0xAB}`), 和 0xDA/0xDC
    的高字节在前**相反**, 别抄错。
  ⚠ 0xAA 已经被按键命令(下发)和按键上报(广播)占用, 所以电流帧用 0xBA 当帧头。

  ⚠ 两条硬限制(上位机和下位机各自把关):
      占空比 **最大 70%**(现场限制); 死区在斩波时 **不小于 100 ns**
      (1..99 ns 等于没插死区, 互补驱动下就是上下管直通), 且**占空比为 0 时死区必须为 0**。

  ---- 上报 ----------------------------------------------------------------

  1) 文本回话(只发给发命令的那个客户端): 一行或几行 ASCII, 例如
       0xA1 start: 5 bridges ON at 30% / brake duty 30 % applied to 5 bridges
       brake 0xA0 enabled (modulation ON); send 0xA1 to output
       0xE0 samp E0 sampling ON (a DATA line every 10 ms) / 0xE1 samp E1 sampling OFF
       0xE2 samp E2 sampling=1 ...
       0xF4 led mask=0x05 on=0,2 freq=100Hz state=normal
       0xAA key ready=1 armed=1 | KEY1(MIO10/E9)=up n=0 | KEY2(MIO11/C6)=up n=0 |
            resets=0 netrestarts=0

  2) 遥测文本广播(所有客户端, 10 ms 一行, 仅在采样且广播打开时):
       DATA <秒> <i1>..<i5> <动子速度> <母线电压> <动子位移> <激光电压>
       例: DATA 1.234 12.345 11.891 12.402 12.010 11.950 123.456 24.310 240.123 0.938
     固定 10 个数(第 1 个是时间), 全部 3 位小数:
       第 2..6 个 = 线圈 1..5 的**电流(A)**
       第 7 个    = **动子速度(mm/s)**: 激光测速, 取绝对值(速率); 丢失目标/静止时报 0
       第 8 个    = **母线电压(V)**(已按 11:1 还原)
       第 9 个    = **动子位移(mm)**: 激光两点标定折出来的位置; 丢失目标时保持上一拍有效值
       第 10 个   = **激光引脚电压(V)**: 排障用(位移/速度都是从它折出来的), 上位机不画
     字段位置是固定契约, 不随序列掩码变; 没采的通道报 0.000。
     ⚠ 新增字段只能加在末尾 —— 插在中间老上位机会整体错位。
     ⚠ 第 7/9 个字段原来分别是"CH6 引脚电压"和"不存在", 2026-11 改成了动子速度/位移;
       上位机的老版本会把第 7 个当电压画, 更新上位机即可。

  3) 二进制广播(所有客户端, 2 字节):
       [0xB0][掩码]  光幕状态变化, bit i = 第 i+1 路被遮挡
       [0xC0][源掩码] 过流出现/恢复, bit i = 第 i+1 个桥报过流(发生与恢复各发一次)
       [0xAA][掩码]  按键变化, bit0 = KEY1 按着, bit1 = KEY2 按着

  ---- 单位与标定(改传感器只改这两处) --------------------------------------

    线圈电流(service_ads.c 的 SVC_ADS_COIL_MA_PER_MV / SVC_ADS_COIL_ZERO_MV):
        I[A] = (1.5 * V[V] - 2.5) * 10   即  I[mA] = 15 * (V[mV] - 零点mV)
        零点默认 1667 mV(1.667 V), 灵敏度 15 A/V; 满量程 5.12 V -> 51.8 A
      ⚠ 零点漂移: 每路传感器的零点差几十 mV, 制动关闭时读数会在 ±0.15 A 上下偏。
        发 **0xE3**(采样中 + 制动关闭)让板子采 2 s 电压, 把这 5 个电压设成各自的归零电压,
        之后静止时 5 路电流就是 0.000 A; 结果会广播一行 `ads zero (mV): ...`。
        零点附近 ±30 mA 的死区会把残余噪声直接归 0。
        序列掩码没使能的通道报 0(不按 0 V 折成 -25 A)
    母线电压(service_ads.c 的 SVC_ADS_VBUS_DIVIDER):
        母线电压 = CH5 引脚电压 * 11
    激光 CH6 -> 动子位移/速度(service_ads.h 的"激光位移标定"那一块, 现场只改它):
        传感器 ST-M2 小型点激光, 模拟输出档位 **4~20 mA**, 板上用 **100 Ω** 取样电阻
        变成电压进 CH6。手册里的电流值 x100 就是板上看到的 mV:
            3.0 mA  -> 300 mV   读数"无效"的错误值(最小) -> 丢目标
            3.8 mA  -> 380 mV   读数"小于映射起点"的余量值 -> 过渡防抖区
           20.2 mA  -> 2020 mV  读数"大于映射终点"的余量值 -> 仍算有效
           21.0 mA  -> 2100 mV  读数"无效"的错误值(最大) -> 丢目标
        两点标定(现场实测): 0.732 V -> 0 mm, 1.143 V -> 480 mm, 直线插值
            x[mm] = x1 + (x2-x1)/(y2-y1) * (V[V] - y1)      即 1 mV ≈ 1.17 mm
        状态机: <300 / >2100 = LOST; 380..2020 = TRACK; 300~380 与 2020~2100 = 保持状态;
                切换要连续 3 拍满足(防抖)。
        测速:   只有上一拍和当前拍都 TRACK 才 v = |Δx|/Δt(Δt = 10 ms); 任何状态跳变都
                清位置缓存和滤波器, 所以不会跨丢失段算假速度; 丢失时速度报 0。
        平滑:   位移一阶低通(alpha 0.35) -> 差分 -> 速度一阶低通(0.30) -> 死区 20 mm/s。
                1 mV≈1.17 mm 这个标定下 10 ms 直接差分有 ±9 mm/s 量化抖动, 所以必须滤;
                死区是为了让"动子停住"时速度干净地读 0(取绝对值会把噪声整流成正的)。
        ⚠ 分辨率: 411 mV 跨 480 mm, ADS 一个 LSB(0.078 mV)≈0.09 mm。想更细就把传感器
          "映射起点/终点"收窄到实际行程, 或者改读它的 RS485 数字量(板上正好有一路 RS485)。

===============================================================================
  制动(0xA0..0xA5 + 0xB0/0xC0)
===============================================================================

  引脚(球号 ↔ FPGA 信号 ↔ 原理图位号; 对照表 vitis2/引脚分配.xlsx, 约束 pwm.xdc):
      桥(CH)  AH=Q1   BH=Q2   AL=Q3   BL=Q4   | 使能 EN1/EN2  | 过流 MAX ILD
      1        W10     V10     V8      W9     | W6     V6     | V11
      2        Y12     R14     Y6      V16    | T10    T11    | Y13
      3        W8      V13     U13     T15    | T12    U12    | T14
      4        V17     P14     V18     P16    | T16    U17    | P15
      5        W16     W19     W18     U18    | R16    R17    | U19

      原理图位号(同一路内): Q1 = LLC_PRI PWM1, Q2 = PWM2, Q3 = PWM3, Q4 = PWM4,
      CH1..CH5 = 第 1..5 个桥。所以每桥的 **W10/V10 是两个上管(Q1/Q2)**、
      **V8/W9 是两个下管(Q3/Q4)**。
      AH/AL 是 A 臂的上下管(Q1/Q3)、BH/BL 是 B 臂的上下管(Q2/Q4);
      对角驱动下 AH 与 BL 同相 —— 空载量波形应该是 **W10 与 W9 同相、V8 与 V10 同相**。
      ⚠ 引脚分配表里 C 列是 FPGA 球号(约束用这个), D 列是转接板丝印, 两者不一定同名
        (例如 gate_en_0[2] 球号 T10、丝印 R1), 接线时别把丝印当球号。

  寄存器: CTRL 0x00 / PERIOD 0x04 / DUTY0..4 0x08..0x18 / DEAD0..4 0x1C..0x2C /
          PWM_EN 0x30([9:0] 半桥使能, [19:10] 每桥腿 B 模式) / STATUS 0x3C /
          PHASE0..4 0x40..0x50 / ID 0x38 = 0x50574D31

  工作方式(app/brake_ctl_app.c, 5 ms 任务):
      每桥 = 腿 A 静态高(Q1 常开 / Q3 常关) + 腿 B(Q2/Q4) 带死区互补斩波
             -> PWM_EN[19:10] = 3 (PWM_AXI_MODE_LEG_A_STATIC_HIGH, IP 2026-10-30 新增)
             -> 负载平均电压 = 占空比% x Vbus, 单向电流、慢衰减, 占空比就是 Q4 的导通占比
      上电    : 五桥输出关、全局调制关、占空比 0%、死区 500 ns(**制动默认关闭**)
      0xA0    : 开全局调制(桥仍关着)
      0xA1    : 五路桥同时打开, 用当前占空比(五路同一个值)
      帧 0xDA : 改占空比(0..70%), 制动开着时立刻同步到五路; 没开则先记住
      帧 0xDC : 改死区(0 或 100..10000 ns), 同样五路同步
      0xA2..0xA4: 预设占空比 30 / 50 / 70%(70 = 上限)
      0xA5    : 五桥全关 + 关全局调制(并清过流锁存)
      过流    : 立刻全关五桥 + 上报 [0xC0]; 要重新制动必须 0xA0 + 0xA1

  频率固定 4 kHz(aclk 50 MHz -> PERIOD 12500); 死区默认 500 ns(=25 周期), 可在线改。

  ⚠ 光幕**不参与任何制动动作**: ls 任务照旧 5 ms 采样, 变化时只广播 [0xB0][掩码]。
    旧那套"光幕逐路触发、五桥分时投入"的时序状态机(app/brake_app.c)保留在仓库里
    但不再建任务、不进命令地图(见那个文件末尾的说明); 需要时把它加回 UserConfig.cmake。
  ⚠ 上电时 PWM_EN[19:10] 被写成 3, 所以空载量波形是: W10(Q1) 常高、V8(Q3) 常低,
    其余两脚(Q2=V10 / Q4=W9)按占空比互补斩波, 中间夹死区。若把模式改回 0(对角驱动),
    才回到"W10 与 W9 同相、V8 与 V10 同相"的那种波形。

  上报: 光幕掩码变化 -> [0xB0][掩码]; 过流出现/恢复 -> [0xC0][源掩码]。

   过流硬件行为(RTL): oc_br_off = ~oc_n | oc_src
      * oc_n[i] 被拉低 -> 该桥 4 管立刻关断(~2 ns), 其余桥继续
      * oc_n 恢复高只去掉实时项, **oc_src 仍把该桥锁住**(粘滞)
      * 清粘滞 = CTRL.GLB_EN 做一次 0->1 边沿(Svc_Oc_Acknowledge)
    APP_PWM_OC_AUTO_ACK = 1 时引脚恢复高就自动确认; 想人工确认改成 0。

===============================================================================
  电流负反馈闭环(0xBA 帧, service/Algorithm_module/)
===============================================================================

  和上面的"占空比模式"是**两条控制链路**, 抢同一个功率级, 规矩是**最后一条命令说了算**:
      收到电流指令 > 0            -> 电流闭环接管(自己开全局调制 + 开五桥)
      0xA5 / 过流 / 收到占空比帧  -> 电流闭环立刻放手(Svc_CurLoop_Stop), 功率级归占空比模式
  两条链路都不会"偷偷"把对方关掉的桥再打开, 也不会在对方刚写好的占空比上盖一个 0。

  链路: net 任务(解析 0xBA 帧) -> Current_App_Command -> Svc_CurLoop_SetTargetA(A)
        curloop 任务(10 ms) -> 每桥一套 PID -> Svc_Pwm_SetBridgeBrakePermille -> PL

  控制律(service_pid.c, 纯 float):
      误差 = 目标电流 - 实际采样电流(A); 输出单位就是占空比 %
      每 10 ms 一拍: 5 个桥各自算, **5 套独立 PID 状态, 绝不共用**
      积分 = 浮点小数累加(不用整数定点模拟)
      抗饱和 = 条件积分: 输出顶到上/下限且误差还在往同方向推时, 这一拍不积分
      爬坡 = 每拍占空比变化不超过 SVC_CURLOOP_SLEW_STEP_PER_CYCLE(默认 2 %/拍)
      目标为 0(< 0.02 A) = 不要电流: 占空比按爬坡速度退回 0, 退干净后关五桥 + 关全局调制

  各桥占空比硬上限(service_current_loop.h 里的显式宏, 不是写死的 100):
      桥1 0..37%   桥2 0..40%   桥3 0..28%   桥4 0..45%   桥5 0..75%

  死区: 闭环**投入的那一刻**把五桥死区设成 100 ns(低占空比段失真最小), 只设一次。
        上位机在 PWM 栏发 0xDC 帧随时可改(0 或 100..10000 ns), 闭环不会每拍刷回 100 ns。

  PID 参数(Kp 1.5 / Ki 3.0 / Kd 0 / 爬坡 2 %/拍)是首版估值, 现场按响应调;
  都在 service_current_loop.h 顶部, 改完重编即可。

  主机测试(不用板子): tools/current_loop_test/run.ps1 —— 把真的 PID 和闭环任务
  拿到 PC 上跑, 逐拍断言上限/爬坡/浮点积分/抗饱和/关闭时序/外部停互锁, 共 68 项。

===============================================================================
  发射小车(0xD0..0xD6, RS-485 上的 Modbus RTU)
===============================================================================

  协议: RS-485 @115200 8N1 + Modbus RTU, 从站 0x01, CRC16 低字节在前;
        应用层是 CiA402(DS402) 伺服行规。**驱动器只接收不回复**, 所以本工程只发不收。

  功能码: 0x06 写单个寄存器(16 位对象) / 0x10 写多个寄存器(32 位对象, 高字在前)

  CiA402 对象: 0x6060 运行模式(1 = PP 轮廓位置) / 0x6040 控制字 / 0x6041 状态字 /
              0x607A 目标位置(pulse) / 0x6064 实际位置 / 0x6081 轮廓速度

  控制字序列: 6 Shutdown -> 7 Switch on -> 0x000F Enable operation -> 0x001F 启动定位
  ⚠ 连续两次定位之间必须**先清 bit4 再置**: 0x000F 然后 0x001F。

  命令:
      0xD0        用当前档位发射
      0xD1..0xD6  选 1..6 档(对应不同 mm/s, 见 app_config.h)并立刻发射
      回话: "mover 0xD3 launch 700 mm/s armed" / "... BUSY" / "... ERR"

  整周期(非阻塞, 由 mover 任务按时刻逐帧推进):
      0x6060=1 / 0x6040=6 / 0x6040=7   (每次发射都重发, 从同一条基线起步)
      等 2000 ms  -> 0x6040=0x000F 使能
      等 200 ms   -> 0x6081=速度
      等 100 ms   -> 0x607A=发射位 + 0x6040=0x001F   去程启动
      等 2100 ms  -> 0x6040=0x000F 回程使能
      等 200 ms   -> 0x607A=0 + 0x6040=0x001F        回程启动
      等 1800 ms  -> 0x6040=0x0006 停机(电机自由)
    正在跑的时候再发 0xDx 回 BUSY, 不打断当前周期; 没有中止命令(要停等它跑完或按 KEY1)。

  ⚠ 位置不做 mm 换算: 0x607A 直接给编译期宏 SVC_MOTOR_LAUNCH_POS 的原始脉冲值。
    手册 1.1.5 说单位是 pulse, 当量取决于负载分辨率(调试软件对象 2301h), 没人确认过;
    要标定就用 0x6064 读一次实际位置(现在没有读寄存器的命令, 只能临时改代码)。

===============================================================================
  ADS8688 采集(0xE0..0xE3) 与 eMMC 录制
===============================================================================

  PS SPI0 接 YK099 模块上的 ADS8688: 8 通道 / 16 位 / 内置 4.096 V 基准。
      CH0..CH4 = 线圈 1..5 的电流传感器    CH5 = 母线    CH6 = 激光    CH7 = 备用
  控制脚: CS = M18(每帧一个脉冲), DAISY = L14(恒低), RST = L15(空闲高)
  SPI: Mode 0, 16 位帧, MSB 在前, SCLK ≈ 1.5625 MHz(参考 200 MHz / 2^(N+1), N=6)
  器件命令: NO_OP 0x0000 / RST 0x8500 / AUTO_RST 0xA000 / MAN_Ch_n 0xC000+n*0x0400;
           命令帧 = 命令 16 位 + 补一帧 NO_OP, 共 4 字节
  程序寄存器: 写 = [(addr<<1)|1, data]; 读 = [addr<<1, 0, 0], **Rx[2] 才是数据**
  电压换算: 双极性 mV = (raw-32767)*跨度/65535; 单极性 mV = raw*跨度/65535

  采样开关由上位机控制: 上电只下发配置、不采样(总线安静); 0xE0 开始, 0xE1 停止。
  每拍 10 ms: 按序列掩码逐路点名(MAN_Ch_n)读回 -> 换算 -> 广播 DATA -> 交给订阅者。

  通道表在 service_ads.c 顶部 gAdsCfg[](每路: 名字 + 量程 + 是否激光), 换传感器/
  换接线只改那张表。量程码: 0=±10.24V 1=±5.12V 2=±2.56V 5=0-10.24V 6=0-5.12V。

  eMMC 录制是**自动**的: 卡挂上 + 采样在跑就录, **没有任何录制命令**。
      数据流: ads 任务 -> Svc_Emmc_OnSample() -> QEmmc(128 条) -> emmc 任务
              -> 组 CSV 行 -> 攒满 512 B 写盘 -> 每 1 s f_sync
      盘上格式(纯 CSV, 拔卡插电脑直接看):
          t_ms,i1,i2,i3,i4,i5,vbus,speed,x_mm,laser_mv
          0,12.345,11.891,12.402,12.010,11.950,24.310,123.456,240.123,937.500
      i1..i5 = 线圈电流(A), vbus = 母线电压(V, 已按 11:1 还原),
      speed = 动子速度(mm/s, 取绝对值), x_mm = 动子位移(mm),
      laser_mv = 激光引脚电压(mV, 排障用);
      t_ms 是相对本次录制起点的毫秒。列顺序和 DATA 行相反(vbus 在前)。
      ⚠ 2026-11 从 8 列扩到 10 列: 老记录(8/9 列)在回读时会被 Emmc_IsDataLine 按
        "字段数不对"丢掉, 只影响回放, 不影响新数据。
      ⚠ 录的是**滤波后**的位移/速度(和 DATA 行、曲线完全一致); 想事后自己重算就得看
        laser_mv 这一列, 用原始电压重新折。
      自动选盘: 首选 1:/data.csv(eMMC, SD1); 不可用时退到 0:/data.csv(SD 卡槽)。
      挂载只在上电做一次(EMCMOUNT 之类的命令已删, 开机后才插卡要重启板子)。

===============================================================================
  光幕 / RS-485 / 按键
===============================================================================

  光幕: LS_I[0..5] = W15 V12 W13 V15 R19 P19(bank 34 EMIO, XDC 有上拉, 低有效)
        5 ms 采样, 连续 3 次同电平才算变化; 变化时广播 [0xB0][掩码] 并打一行日志。
        注意 raw 是全高 = 没遮挡; raw 一直 0x00 说明接线错或极性反了。

  RS-485: tx = D19 -> 收发器 DI, rx = D20 <- RO, de = G14(高 = 发送)
        本组件只提供"发一段字节"(阻塞式半双工)+ 递归总线锁, 没有协议也没有接收。
        中断在 rs485 任务里连(顺序原因见"踩过的坑"2)。

  按键: KEY1 = MIO10(E9) -> 软复位;  KEY2 = MIO11(C6) -> 重启 TCP 服务端
        20 ms 轮询 + 3 次消抖; 上电 3 s 内不响应(防按键卡住变成起来就复位);
        只在"消抖后电平跳到按下"那一次动作。KEY2 的动作可由
        APP_KEY2_RESTART_ENABLE 关掉(引脚照读照计数, 只是不动作)。

===============================================================================
  编译 / 回归 / 烧写
===============================================================================

  Vitis IDE: File -> Open Workspace -> 选 vitis2/ 目录; 先 Build 平台 soc_923
             (改过 BSP 配置时必须), 再 Build app923; Run -> 下载 app923.elf

  命令行:
     set PATH=D:\vivado\Vitis\2024.2\gnu\aarch32\nt\gcc-arm-none-eabi\bin;%PATH%
     D:\vivado\Vitis\2024.2\tps\mingw\10.0.0\win64.o\nt\bin\ninja.exe
       (在 vitis2\app923\build 目录下执行)
  编译选项里有 -Wall -Wextra -Wundef: 目标是**零警告**。

  上位机(Qt6):
     build\Desktop_Qt_6_11_2_MinGW_64_bit_Debug 下
     D:\QT\Tools\mingw1310_64\bin\mingw32-make.exe -j8

  改代码之后先跑回归(PC 上跑, 不需要板子):
     powershell -ExecutionPolicy Bypass -File tools\led_test\run.ps1         灯:掩码/频率/命令(314 项)
     powershell -ExecutionPolicy Bypass -File tools\pwm_logic_test\run.ps1   PWM 寄存器编码
     powershell -ExecutionPolicy Bypass -File tools\emmc_test\run.ps1        CSV 组行/整行装配/条数
     powershell -ExecutionPolicy Bypass -File tools\key_test\run.ps1         按键消抖/上电保护期
     powershell -ExecutionPolicy Bypass -File tools\check_invariants.ps1     分层不变量 + 启动链

  这些都是纯逻辑测试: 把组件的 .c 编进 PC 上的测试主程序, 用假寄存器/假 FatFs 顶掉
  硬件访问 —— 断言的是"代码的意图", 板上那一半仍然要实测。

  改过硬件 / 换过 XSA 之后(烧写前必做):
     powershell -ExecutionPolicy Bypass -File tools\refresh_fsbl_psinit.ps1
       刷新 FSBL 的 ps7_init.c/.h -> 重建 FSBL -> 重生成 BOOT.bin -> 自检

  烧写: BOOT.bin 在 vitis2/app923/flash/, 用 Vitis 的 Program Flash 烧; 上电横幅
        "[built <日期> <时间>]" 可以确认板上跑的是哪一版。

===============================================================================
  配置文件注意事项(改硬件 / 换 XSA 之后必看)
===============================================================================

  按目录分所有权:

    平台 / BSP(工具管, 重新生成平台就刷新):
      vitis2/soc_923/hw/sdt/*                        硬件交接(ps7_init.c/.h/.tcl)
      vitis2/soc_923/zynq_fsbl/zynq_fsbl_bsp/hw_artifacts/*
      vitis2/app923/_ide/psinit/ps7_init.tcl         JTAG 调试初始化 PS 用

    应用目录(工具不覆盖):
      vitis2/soc_923/zynq_fsbl/*.c *.h *.ld          FSBL 源码
      vitis2/app923/src/*                            本工程源码

    应用目录里会被工具重写的少数文件:
      app.yaml / CMakeLists.txt / UserConfig.cmake / compile_commands.json

  ⚠ FSBL 的 ps7_init.c/.h 是"没人管的死角": 它们是 Vitis **创建应用那一刻**从当时的
    硬件交接填进去的, 之后"重新生成平台"只刷新平台/BSP 侧, 不会回头更新 FSBL 应用目录
    里那份 —— 于是 **JTAG 跑新配置、flash 跑旧配置**。2026/10/04 就是这样把 IO PLL 配成
    1800 MHz(而 app 按 1000 MHz 硬编码分频), 结果网口"有链接、MDIO 正常、一个包都过不去"。
    换硬件后一定要跑 tools\refresh_fsbl_psinit.ps1(它会比对、覆盖、重建、重打 BOOT.bin)。

===============================================================================
  踩过的坑(别改回去)
===============================================================================

  1. SCU 私有定时器属于 FreeRTOS(它就是 tick 源)。谁再 XScuTimer_CfgInitialize 它,
     谁就把调度器心跳弄坏 —— enet 平台层只走 OSAL 的时间接口。

  2. GIC 会被重新初始化。顺序必须是: Enet_Init(内含 lwIP 的 GIC 初始化) -> 启动调度器
     -> 需要连中断的驱动在**任务里**再连(rs485 就是这么做的)。

  3. 日志要整行加锁, 而且必须是**递归**互斥量: 监视任务会把几行圈成一段, 段内又调 LOG_*。

  4. GEM0 参考时钟: SDT 流程里 lwIP 的 SetUpSLCRDivisors() 被编译掉了,
     Enet_Phy_ApplyClock() 只按 "IO PLL = 1000 MHz" 算分频, 修不了"PLL 本身配错"。

  5. PWM 引脚只能写 bank 13/34/35 的合法球名; R0~R7 / G0~G7 / B0~B7 不存在或属于 PS,
     写进约束会让 Vivado 报错 / DRC 挡住位流。可用清单: tools/package_pins.txt

  6. 光幕任务曾经"根本没被创建": brake_app.c 里写了 #if (APP_ENABLE_LS != 0U), 但那个
     文件没有 include app_config.h —— 未定义标识符在 #if 里当 0, 整段(包括
     Osal_TaskCreate("ls"))被悄悄编译掉, 函数还照样返回成功。现象是"光幕置 0 不识别、
     不上报、时序不推进"。现在改成用 Svc_Ls_IsReady() 判断, 并且编译选项加了 -Wundef,
     以后再犯会直接报警告。

  7. 换硬件 / 换 XSA 之后 FSBL 的 ps7_init.c/.h 不会自动更新(见上一节)。

  8. 分层不是摆设: bsp/ 不许 include app/ 或 service/; service/ 不许 include app/;
     app/ 要用硬件只经 service; 除 osal_freertos.c 外任何文件都不许直接调 FreeRTOS API。
     tools\check_invariants.ps1 会自动检查这四条。

  9. **-DSDT 不能删**(UserConfig.cmake): 它把整个 BSP 头文件族切成 SDT 形态,
     XGpioPs_LookupConfig() 收的是**基地址**而不是 DeviceId。BSP 库自己就是带 -DSDT 编的,
     少了它不只是编不过, 而是两种 ABI 混用。

  10. eMMC 三条: 挂载时不要用 FA_OPEN_ALWAYS 打开 data.csv(空卡会被凭空建出文件);
      条数按 f_size 算而不是内存计数器(复位后卡上数据还在); FSBL 的 ps7_init 必须使能 SD1
      (否则 flash 启动挂不上 eMMC 而 JTAG 正常)。

  11. 呼吸灯的端口名是 led[0..3], 不是 led_0。XDC 里写错一个名字会让整份约束失效 ——
      改 XDC 之后一定要跑一遍实现。

  12. 单字节命令是"扫整条报文里的这些字节", 所以: 参数必须和命令在**同一个报文**里
      (0xFE / 0xFF 吃下一个字节); 不要往报文里塞可能撞上命令字节的二进制数据。

  13. 别用 PowerShell 的 Set-Content 改源码。Windows PowerShell 的 -Encoding UTF8 会把
      已经是 UTF-8 的中文再解释一遍, 结果是 mojibake(踩过)。
