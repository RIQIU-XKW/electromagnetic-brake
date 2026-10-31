# 目录地图与关键 API

## 固件（`src/sw_ps`）

```
main.c                 Log_Init → App_Init → App_StartTasks → Osal_StartScheduler
app/
  app.c/.h             装配：初始化顺序、命令地图注册、App_StartTasks、上电自检
  app_config.h         全部应用策略（DHCP/端口/监视周期/过流自动确认/APP_ENABLE_*/按键参数）
  inter_app.c/.h       命令地图 Inter_App_Commands[] + STATUS + 按键 0xAA..0xAC + net/key 任务
  brake_ctl_app.c/.h   制动控制（在用）：五路同步、帧 0xDA/0xDC、0xA0..0xA5、光幕/过流上报、brkctl 任务
  brake_app.c/.h       旧"光幕逐路触发"时序状态机：保留在仓库、仍参与编译，但**不建任务、不进命令地图**
  current_app.c/.h     电流闭环：解析 0xBA 帧 → `Svc_CurLoop_SetTargetA()`，建 curloop 任务
  Mover_app.c/.h       发射小车 0xD0..0xD6 + 整周期时序 + mover 任务
  samp_app.c/.h        采样 0xE0..0xE2
  monitor_app.c/.h     呼吸灯 0xF0..0xFF + led/mon 任务
service/
  service.h            Svc_StatusTypeDef（SVC_OK / ERR_PARAM / ERR_FULL / ERR_HW / ERR_STATE）
  Algorithm_module/    电流闭环算法（纯算法，不碰寄存器）
                       service_pid(浮点积分 + 条件积分抗饱和 + 爬坡限速)
                       service_current_loop(5 套独立 PID + 10 ms 任务 + 与占空比模式的互锁)
  inter_module/        service_net(net 任务/广播队列/快照/客户端钩子) net_cmd(注册表+分发)
                       net_text(不用 sprintf 的拼接) service_key(按键消抖+上报回调)
  brake_module/        service_pwm(频率/死区/占空比/全局调制/过流状态/五路同步/千分比接口)
                       service_ls(光幕 6 路采样+消抖) service_oc(过流状态+事件位+acknowledge)
  Samp_module/         ads8688(纯逻辑协议层) service_ads(10 ms 扫描+换算+激光状态机+DATA 广播)
                       service_emmc(样本队列+CSV 10 列+512 B 暂存+1 s f_sync)
  Mover_module/        service_rs485(半双工发送+递归总线锁) modbus_rtu(组帧+CRC16)
                       motor_cia402(寄存器/控制字/单位) service_motor(发帧动作)
  monitor_module/      service_monitor(周期日志+过流巡检) led/service_led(队列+4 路掩码)
                       log/log.h(LOG_I/W/E + 整行递归锁)
osal/  osal.h, osal_freertos.c        任务/队列/递归互斥量/时间/中断；唯一允许 FreeRTOS 符号
bsp/   enet/ pwm/ led/ ls/ oc/ rs485/ ps_spi/ emmc/ ps_key/ ps_rst
```

## 十一个任务

| 任务 | 创建处 | prio / 栈 | 周期 | 干什么 |
|---|---|---|---|---|
| net | `Inter_App_Start()` | 4 / 6 KB | 2 ms | 独占 lwIP，收发 + 命令分发 + 广播队列 |
| key | `Inter_App_Start()` | 2 / 1.5 KB | 20 ms | 两个 PS 按键消抖 + 复位/重连 |
| led | `Monitor_App_Start()` | 2 / 1.5 KB | 队列 | 4 路呼吸灯 |
| mon | `Monitor_App_Start()` | 1 / 2 KB | 1 s | 状态日志 + 过流巡检 |
| ls | `BrakeCtl_App_Start()` | 2 / 1.5 KB | 5 ms | 光幕采样 + 消抖 |
| brkctl | `BrakeCtl_App_Start()` | 3 / 2 KB | 5 ms | 应用占空比/死区 + 光幕/过流上报 |
| rs485 | `Mover_App_Start()` | 3 / 4 KB | 起端口后睡 | 只在任务里 `Start` 端口（GIC 顺序要求） |
| mover | `Mover_App_Start()` | 2 / 2 KB | 时序等待 | 发射整周期推进 |
| ads | `Samp_App_Start()` | 3 / 2.5 KB | 10 ms | 独占 PS SPI0 读 ADS8688 |
| emmc | `Samp_App_Start()` | 2 / 6 KB | 队列 + 1 s | CSV 落盘 |
| curloop | `Current_App_Start()` | 3 / 2 KB | 10 ms | 5 路独立 PID 电流闭环 |

⚠ **两条控制链路抢同一个功率级**：`brkctl`（占空比模式）和 `curloop`（电流闭环）谁都不许
"偷偷"关掉对方的桥再打开，规矩是最后一条命令说了算，互锁全部集中在
`Svc_CurLoop_Stop()`（被 `BrkCtl_AllOff`、过流分支、`0xA1`、占空比帧四处调用）。

守卫用的 `IsReady()`：`Svc_Net_IsReady` / `Svc_Key_IsReady` / `Svc_Led_IsReady` / `Svc_Ls_IsReady` /
`Svc_Rs485_IsReady` / `Svc_Ads_IsReady` / `Svc_Emmc_IsReady`。
`service_pwm` 没有 `IsReady()`，用 **`Svc_Pwm_GetHandle() == NULL`** 判断是否初始化过。

## 初始化顺序（`App_Init`，不能乱）

1. `Svc_Net_Init()`（lwIP + GEM0；它会做第一次 GIC 初始化，清空处理器表 → 必须最先）
2. `Svc_Led_Init()`，随后 `Svc_Net_SetClientHook(App_OnClientCount)`（**必须在建 net 任务之前**）
3. `BrakeCtl_App_Init()`：内部依次 `Svc_Pwm_Init()` → `Svc_Pwm_CheckId()` → 五路桥配成
   `PWM_AXI_MODE_LEG_A_STATIC_HIGH` + 占空比 0 + 死区 500 ns → 桥全关 + 关调制 → `Svc_Ls_Init()` → `Svc_Oc_Init()`
3b. `Current_App_Init()`：`Svc_CurLoop_Init()`（5 套 PID，各桥上限 37/40/28/45/75 %）
4. rs485 / ads / motor / emmc（失败只打一行，不阻断整体；emmc 要在 `Samp_App_Start` 之前挂样本回调）

`App_OnClientCount(0 → ≥1)` 会调 `BrakeCtl_App_ForceOff()`：**上位机连上就把制动强制关掉**
（默认态是不制动，上一次试验留下的输出不许跟着新连接继续跑）。

## 关中断与 RTOS 的约定

- 中断里只做"搬字节 + 叫醒任务"，协议解析一律在任务上下文。
- 连中断必须放在**调度器起来之后的任务里**（`Svc_Rs485_Task` 里才 `Rs485_Start()`）：
  第一次连中断会重填 GIC 处理器表。
- SCU 私有定时器是 FreeRTOS 的 tick 源，`bsp/enet` 的时间基准只走 OSAL，别再配置它。
