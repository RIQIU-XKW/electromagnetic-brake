`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Module Name : tb_pwm
// Description : pwm_axi_ip 的**看波形用** testbench (edit_pwm_axi_ip_v1_0 工程)
//
//   工作点 (按实际电机驱动参数):
//     时钟        s00_axi_aclk = 50 MHz, 1 拍 = 20 ns
//     PWM 频率    4 kHz  ->  PERIOD = 50e6 / 4000 = 12500 拍 = 250 us
//     死区        100 ns ->  DEAD   = 100 / 20    = 5 拍
//     占空比      从 10% 起每档 +10%, 到 100% 为止 (DUTY 不超过 PERIOD)
//
//   !! 看波形的关键提醒 !!
//     一个周期 250 us, 而死区只有 100 ns, 占周期 0.04%。这两个尺度**不可能同屏**:
//       - 看占空比 / 相位   -> 时间轴缩到 ~300 us (一屏 1~2 个周期)
//       - 看死区细节        -> 时间轴缩到 ~1 us (死区占屏幕宽度 10%, 清楚可见)
//     死区开关量小是<正常>的, 真实 4 kHz 驱动就是这样。
//
//   被测结构: 5 个 H 桥, 每桥 4 个开关, 共 20 路 PWM
//     桥 i (i = 0..4):
//       pwm_out[4*i+0] = AH  A 桥臂上管     pwm_out[4*i+1] = AL  A 桥臂下管
//       pwm_out[4*i+2] = BH  B 桥臂上管     pwm_out[4*i+3] = BL  B 桥臂下管
//       gate_en[2*i+0] = A 桥臂驱动使能     gate_en[2*i+1] = B 桥臂驱动使能
//     对角导通(lock anti-phase): AH 与 BL 同相, AL 与 BH 同相
//     每个桥臂的死区独立可配: DEADx = {B 桥臂[31:16], A 桥臂[15:0]}
//
//   仿真设置: SIM_1 的 top 必须是 tb_pwm, 运行时间 xsim.simulate.runtime 必须是 all。
//
//   激励分段 (段间留了间隔, 波形窗口里按时间跳转即可):
//     P0 复位                  20 路输出全低
//     P1 五桥不同占空比        10/30/50/70/90 %
//     P2 桥0 占空比递增        10% -> 100%, 每档保持一个完整周期
//     P3 桥0 占空比 100%       上管除死区外整周期导通, 下管全关
//     P4 两臂死区独立          A 臂 100 ns / B 臂 300 ns (验证完恢复 100 ns)
//     P5 单腿使能关闭          清 PWM_EN[0] -> 只有桥0 A 臂两路变低
//     P6 全局使能关闭          GLB_EN=0, 20 路全低, gate_en 不变
//     P7 INV_PWM 反相          20 路电平整体翻转
//     P8 PERIOD=0              周期无效, 20 路全低
//     P9 过流保护              oc_n[0] 拉低 -> 1 ns 内**只有桥 0** 关断, 桥 1~4 照常跑
//     P10 相位偏移            5 个桥错开 0/72/144/216/288 度, 桥臂内互补不受影响
//     P11 涡流制动            线圈 (感性负载) 的 5 个状态 + L-R 模型算出的线圈电流
//
//   过流输入 oc_n[4:0] : 每桥一路, 低有效, IP 内部上拉。本 tb 平时保持 5'h1F。
//////////////////////////////////////////////////////////////////////////////////

module tb_pwm;

    //------------------------------------------------------------------
    // 工作点常量
    //------------------------------------------------------------------
    localparam integer CLK_HZ  = 50000000;          // s00_axi_aclk
    localparam integer PWM_HZ  = 4000;              // 目标 PWM 频率
    localparam integer PERIOD  = CLK_HZ / PWM_HZ;   // 12500 拍 = 250 us
    localparam integer DEAD    = 100 / 20;          // 100 ns / 20 ns = 5 拍

    // 百分比 -> 比较值, 保证不超过 PERIOD
    function integer pct2duty;
        input integer pct;
        integer d;
        begin
            d = (PERIOD * pct) / 100;
            if (d > PERIOD) d = PERIOD;
            pct2duty = d;
        end
    endfunction

    //------------------------------------------------------------------
    // 时钟与复位 : 50 MHz, 和 PS 的 FCLK_CLK0 一致 (1 拍 = 20 ns)
    //------------------------------------------------------------------
    reg aclk    = 1'b0;
    reg aresetn = 1'b0;

    always #10 aclk = ~aclk;        // 20 ns 周期

    //------------------------------------------------------------------
    // AXI4-Lite 主机信号
    //------------------------------------------------------------------
    reg  [6:0]  awaddr  = 7'd0;
    reg  [2:0]  awprot  = 3'd0;
    reg         awvalid = 1'b0;
    wire        awready;
    reg  [31:0] wdata   = 32'd0;
    reg  [3:0]  wstrb   = 4'hF;
    reg         wvalid  = 1'b0;
    wire        wready;
    wire [1:0]  bresp;
    wire        bvalid;
    reg         bready  = 1'b0;

    reg  [6:0]  araddr  = 7'd0;
    reg  [2:0]  arprot  = 3'd0;
    reg         arvalid = 1'b0;
    wire        arready;
    wire [31:0] rdata;
    wire [1:0]  rresp;
    wire        rvalid;
    reg         rready  = 1'b0;

    //------------------------------------------------------------------
    // DUT 输出
    //------------------------------------------------------------------
    wire [19:0] pwm_out;
    wire [9:0]  gate_en;

    // oc_n[4:0] 低有效, 1 = 正常。平时拉高, 免得误触发保护。
    reg  [4:0]  oc_n = 5'h1F;

    // 寄存器偏移 (7 位地址总线 -> 128 字节窗口)
    localparam [6:0] A_CTRL   = 7'h00,
                     A_PERIOD = 7'h04,
                     A_DUTY0  = 7'h08,
                     A_DUTY1  = 7'h0C,
                     A_DUTY2  = 7'h10,
                     A_DUTY3  = 7'h14,
                     A_DUTY4  = 7'h18,
                     A_DEAD0  = 7'h1C,
                     A_DEAD1  = 7'h20,
                     A_DEAD2  = 7'h24,
                     A_DEAD3  = 7'h28,
                     A_DEAD4  = 7'h2C,
                     A_PWM_EN = 7'h30,
                     A_PHASE0 = 7'h40,
                     A_PHASE1 = 7'h44,
                     A_PHASE2 = 7'h48,
                     A_PHASE3 = 7'h4C,
                     A_PHASE4 = 7'h50,
                     A_ID     = 7'h38;

    //------------------------------------------------------------------
    // 被测 IP
    //------------------------------------------------------------------
    pwm_axi_ip dut (
        .pwm_out            (pwm_out),
        .gate_en            (gate_en),
        .oc_n               (oc_n),
        .s00_axi_aclk       (aclk),
        .s00_axi_aresetn    (aresetn),
        .s00_axi_awaddr     (awaddr),
        .s00_axi_awprot     (awprot),
        .s00_axi_awvalid    (awvalid),
        .s00_axi_awready    (awready),
        .s00_axi_wdata      (wdata),
        .s00_axi_wstrb      (wstrb),
        .s00_axi_wvalid     (wvalid),
        .s00_axi_wready     (wready),
        .s00_axi_bresp      (bresp),
        .s00_axi_bvalid     (bvalid),
        .s00_axi_bready     (bready),
        .s00_axi_araddr     (araddr),
        .s00_axi_arprot     (arprot),
        .s00_axi_arvalid    (arvalid),
        .s00_axi_arready    (arready),
        .s00_axi_rdata      (rdata),
        .s00_axi_rresp      (rresp),
        .s00_axi_rvalid     (rvalid),
        .s00_axi_rready     (rready)
    );

    //------------------------------------------------------------------
    // 给波形窗口准备的"人看的"别名: 每桥 4 位 = {BL, BH, AL, AH}
    //   一条就能看出"对角同相"和"桥臂内互补"
    //------------------------------------------------------------------
    wire [3:0] hb0 = pwm_out[3:0];      // 桥 0 : bit0=AH bit1=AL bit2=BH bit3=BL
    wire [3:0] hb1 = pwm_out[7:4];      // 桥 1
    wire [3:0] hb2 = pwm_out[11:8];     // 桥 2
    wire [3:0] hb3 = pwm_out[15:12];    // 桥 3
    wire [3:0] hb4 = pwm_out[19:16];    // 桥 4

    // 桥 0 的四个开关单独拉出来, 看死区最方便
    wire ah = pwm_out[0];
    wire al = pwm_out[1];
    wire bh = pwm_out[2];
    wire bl = pwm_out[3];

    //------------------------------------------------------------------
    // 涡流制动线圈的简化模型  (P11 段用)
    //
    //   只做一件事: 把 4 个引脚的电平翻译成"线圈看到的电压", 再逐拍积分成
    //   电流。这样波形上能直接看电流形状, 而不只是一堆高低电平。
    //   它不参与任何功能验证, 只是给人看的。
    //
    //   桥输出电压由引脚决定:
    //     AH & BL          -> +Vbus          正向驱动
    //     AL & BH          -> -Vbus          反向驱动 / 反向快衰减
    //     AL & BL          ->  0V            两个下管短路  -> 只靠 I*R 衰减(慢)
    //     AH & BH          ->  0V            两个上管短路到母线
    //     全关(滑行)        -> -Vbus (i>0)    体二极管续流, 能量回灌母线 -> 快衰减
    //                          +Vbus (i<0)
    //     单个管子导通      ->  0V            (死区那种情况, 近似)
    //
    //   注意"全关"不能当成 0V: 体二极管会把线圈端电压钳到母线上, 所以滑行
    //   和反向电压一样是快衰减, 区别只是能量去了哪里。真正慢的只有主动短路。
    //
    //   电流按 di/dt = (v - i*R) / L 用 20 ns 步长积分。
    //   想换成你自己的线圈, 只改这三个 localparam。
    //------------------------------------------------------------------
    localparam real VBUS   = 24.0;      // 母线电压 [V]
    localparam real COIL_L = 1.0e-3;    // 线圈电感 [H]
    localparam real COIL_R = 2.0;       // 线圈电阻 [ohm]
    localparam real DT_S   = 20.0e-9;   // 1 拍 = 20 ns

    real v_coil;
    real i_coil;
    real i_peak;

    always @(*)
    begin
        if      (ah && bl) v_coil =  VBUS;
        else if (al && bh) v_coil = -VBUS;
        else if (al && bl) v_coil =  0.0;                        // 两个下管
        else if (ah && bh) v_coil =  0.0;                        // 两个上管
        else if (!ah && !al && !bh && !bl)                       // 全关: 二极管续流
            v_coil = (i_coil > 0.0) ? -VBUS : VBUS;
        else               v_coil =  0.0;                        // 单管导通
    end

    always @(posedge aclk)
    begin
        if (aresetn === 1'b1)
            i_coil = i_coil + (v_coil - i_coil * COIL_R) * DT_S / COIL_L;
        else
            i_coil = 0.0;
    end

    //------------------------------------------------------------------
    // AXI4-Lite 写 : BREADY 一直保持到写响应被取走, 否则从机 BVALID
    //                不会撤销, 下一笔事务握手就错位了
    //------------------------------------------------------------------
    task axi_write;
        input [6:0]  addr;
        input [31:0] data;
        begin
            @(negedge aclk);
            while (!(awready && wready)) @(negedge aclk);
            awaddr  = addr;
            awprot  = 3'b000;
            awvalid = 1'b1;
            wdata   = data;
            wstrb   = 4'hF;
            wvalid  = 1'b1;
            bready  = 1'b1;
            @(negedge aclk);            // 上升沿被从机接收
            awvalid = 1'b0;
            wvalid  = 1'b0;
            while (!bvalid) @(negedge aclk);
            @(negedge aclk);            // 取走写响应
            bready  = 1'b0;
            @(negedge aclk);
        end
    endtask

    task axi_read;
        input  [6:0]  addr;
        output [31:0] data;
        begin
            @(negedge aclk);
            while (!arready) @(negedge aclk);
            araddr  = addr;
            arprot  = 3'b000;
            arvalid = 1'b1;
            rready  = 1'b1;
            @(negedge aclk);
            while (!rvalid) @(negedge aclk);
            data    = rdata;
            arvalid = 1'b0;
            @(negedge aclk);
            rready  = 1'b0;
            @(negedge aclk);
        end
    endtask

    task wait_clk;
        input integer n;
        integer i;
        begin
            for (i = 0; i < n; i = i + 1) @(negedge aclk);
        end
    endtask

    task wait_periods;
        input integer n;
        integer i;
        begin
            for (i = 0; i < n; i = i + 1) wait_clk(PERIOD);
        end
    endtask

    //------------------------------------------------------------------
    // 配置一个桥: 占空比(拍) + 两个桥臂各自的死区(拍)
    //------------------------------------------------------------------
    task cfg_bridge;
        input integer i;            // 桥号 0..4
        input integer duty;
        input integer dead_a;
        input integer dead_b;
        begin
            axi_write(A_DUTY0 + 7'd4*i, duty);
            axi_write(A_DEAD0 + 7'd4*i, {dead_b[15:0], dead_a[15:0]});
        end
    endtask

    task phase;
        // 注意: 宽度要够, 否则长标题会被从左边截断 (Verilog 字符串赋给
        //       窄端口时保留低位), 日志里就会丢掉 "P4  ..." 这样的前缀
        input [8*96-1:0] name;
        begin
            $display("");
            $display("========================================================");
            $display("  %0s   @ %0t", name, $time);
            $display("========================================================");
        end
    endtask

    reg [31:0] rd;
    integer    p;
    integer    i;
    integer    k;               // P11 用的循环计数
    real       t_fast_us;       // P11 记录反向快释放用掉的时间

    // 单个桥的涡流制动状态配置 (只用桥 0 驱动线圈, 其余 4 桥关掉, 波形干净)
    //   PWM_EN = {12'd0, modes[9:0], enables[9:0]}
    //     enables = 10'b0000000011  -> 只开桥 0 的两个半桥
    //     modes   = {8'd0, mode_b}  -> 桥 0 的 B 臂模式
    task coil_state;
        input integer duty;
        input integer dead_a;
        input integer mode_b;
        begin
            axi_write(A_DUTY0, duty);
            axi_write(A_DEAD0, {16'd0, dead_a[15:0]});   // B 臂死区静态时被忽略
            axi_write(A_PWM_EN, {12'd0, 8'd0, mode_b[1:0], 10'b0000000011});
        end
    endtask

    //------------------------------------------------------------------
    // 激励
    //------------------------------------------------------------------
    initial begin
        $timeformat(-9, 3, " ns", 12);
        $dumpfile("tb_pwm.vcd");
        $dumpvars(1, tb_pwm);               // tb 层信号: pwm_out/gate_en/oc_n
        // 波形别名 hb0~hb4 / ah~bl 只给波形窗口看, 代码里不引用, 因此
        // elaboration 有可能把它们优化掉 (实测: 不带 -debug 时确实会消失)。
        // 显式把它们列进 dump 列表, 从"没人读"变成"有消费者", 就不会被优化掉,
        // 也就不再依赖 xelab 的 -debug 等级。
        $dumpvars(0, tb_pwm.hb0);
        $dumpvars(0, tb_pwm.hb1);
        $dumpvars(0, tb_pwm.hb2);
        $dumpvars(0, tb_pwm.hb3);
        $dumpvars(0, tb_pwm.hb4);
        $dumpvars(0, tb_pwm.ah);
        $dumpvars(0, tb_pwm.al);
        $dumpvars(0, tb_pwm.bh);
        $dumpvars(0, tb_pwm.bl);

        $display("");
        $display("########################################################");
        $display("#  pwm_axi_ip 波形演示  5 个 H 桥 / 20 路 PWM");
        $display("#  时钟 50 MHz, PWM 4 kHz, PERIOD = %0d 拍 = %0d ns", PERIOD, PERIOD*20);
        $display("#  死区 100 ns = %0d 拍", DEAD);
        $display("#  看占空比: 时间轴 ~300 us   看死区: 时间轴 ~1 us");
        $display("########################################################");

        //--------------------------------------------------------------
        // P0 复位
        //--------------------------------------------------------------
        phase("P0  reset : all 20 outputs must be LOW");
        aresetn = 1'b0;
        wait_clk(10);
        aresetn = 1'b1;
        wait_clk(20);
        $display("  pwm_out = %b", pwm_out);
        $display("  gate_en = %b   (both must be 0)", gate_en);

        //--------------------------------------------------------------
        // P1 五个桥不同占空比, 两臂死区都是 100 ns
        //--------------------------------------------------------------
        phase("P1  5 bridges: DUTY = 10/30/50/70/90 %, DEAD = 100 ns");
        axi_write(A_CTRL,   32'd0);                  // 先关调制
        axi_write(A_PERIOD, PERIOD[31:0]);
        cfg_bridge(0, pct2duty(10), DEAD, DEAD);
        cfg_bridge(1, pct2duty(30), DEAD, DEAD);
        cfg_bridge(2, pct2duty(50), DEAD, DEAD);
        cfg_bridge(3, pct2duty(70), DEAD, DEAD);
        cfg_bridge(4, pct2duty(90), DEAD, DEAD);
        axi_write(A_PWM_EN, 32'h0000_03FF);          // 10 个半桥全部使能
        axi_write(A_CTRL,   32'h0000_0001);          // GLB_EN=1 开始调制
        wait_periods(2);

        $display("  -> PERIOD=%0d 拍, 上管导通 = DUTY-DEAD:", PERIOD);
        $display("     10%%: %0d - %0d = %0d 拍", pct2duty(10), DEAD, pct2duty(10)-DEAD);
        $display("     30%%: %0d - %0d = %0d 拍", pct2duty(30), DEAD, pct2duty(30)-DEAD);
        $display("     50%%: %0d - %0d = %0d 拍", pct2duty(50), DEAD, pct2duty(50)-DEAD);
        $display("     70%%: %0d - %0d = %0d 拍", pct2duty(70), DEAD, pct2duty(70)-DEAD);
        $display("     90%%: %0d - %0d = %0d 拍", pct2duty(90), DEAD, pct2duty(90)-DEAD);
        $display("  -> 5 个桥共用周期计数器, 相位完全对齐; AH 与 BL 同相导通");

        //--------------------------------------------------------------
        // P2 桥 0 占空比从 10% 递增到 100%, 每档保持一个完整周期
        //--------------------------------------------------------------
        phase("P2  bridge0 DUTY ramp  10 % -> 100 %, step 10 %");
        for (p = 1; p <= 10; p = p + 1) begin
            axi_write(A_DUTY0, pct2duty(10*p));
            $display("  桥0 占空比 = %3d %%   DUTY = %0d 拍   上管导通 %0d 拍   @ %0t",
                     10*p, pct2duty(10*p), pct2duty(10*p)-DEAD, $time);
            wait_periods(1);
        end
        $display("  -> 缩到 ~300 us 看: 高电平宽度单调递增, 周期始终 250 us 不变");

        //--------------------------------------------------------------
        // P3 占空比 100%: 上管除死区外整周期导通
        //--------------------------------------------------------------
        phase("P3  bridge0 DUTY = 100 % : high side on except the dead time");
        axi_write(A_DUTY0, PERIOD[31:0]);
        wait_periods(2);
        $display("  -> DUTY = PERIOD = %0d: 上管导通 %0d 拍(占比 %.2f%%), 下管全程关断",
                 PERIOD, PERIOD-DEAD, 100.0*(PERIOD-DEAD)/PERIOD);
        $display("  -> 缩到 ~1 us 看任何一个周期的起点: 就是那 %0d 拍死区", DEAD);

        //--------------------------------------------------------------
        // P4 两个桥臂的死区可以不同 (验证完恢复成 100 ns)
        //--------------------------------------------------------------
        phase("P4  LEG A = 100 ns (5 counts)  vs  LEG B = 300 ns (15 counts)");
        cfg_bridge(0, pct2duty(50), DEAD, 15);
        wait_periods(2);
        $display("  -> A 臂死区 5 拍, B 臂死区 15 拍; 缩到 ~1 us 对比两个边沿");
        $display("  -> 上下管关断时间不同时, 两臂死区分开设才有意义");
        cfg_bridge(0, pct2duty(50), DEAD, DEAD);     // 恢复成 100 ns
        wait_periods(1);

        //--------------------------------------------------------------
        // P5 单腿使能关闭
        //--------------------------------------------------------------
        phase("P5  clear PWM_EN[0] : only bridge0 LEG A pins go low");
        axi_write(A_PWM_EN, 32'h0000_03FE);          // bit0 = 0 -> 桥0 A 臂使能关
        wait_periods(2);
        axi_write(A_PWM_EN, 32'h0000_03FD);          // bit1 = 0 -> 桥1 A 臂使能关
        wait_periods(2);
        axi_write(A_PWM_EN, 32'h0000_03FB);   // bit2 = 0 -> 桥1 A 臂关
          wait_periods(2);
        $display("  -> AH/AL 恒低, BH/BL 照常; gate_en[0]=0, 其余 9 路不变");

        //--------------------------------------------------------------
        // P6 全局使能关闭
        //--------------------------------------------------------------
        phase("P6  GLB_EN=0 : all 20 PWM outputs forced LOW");
        axi_write(A_PWM_EN, 32'h0000_03FF);          // 恢复全部使能
        axi_write(A_CTRL,   32'h0000_0000);          // GLB_EN=0
        wait_periods(1);
        $display("  -> pwm_out 全 0, 但 gate_en 仍是 3FF (两者故意解耦)");

        //--------------------------------------------------------------
        // P7 INV_PWM 反相
        //--------------------------------------------------------------
        phase("P7  INV_PWM=1 : all 20 output levels inverted");
        axi_write(A_CTRL, 32'h0000_0003);            // GLB_EN=1 + INV_PWM=1
        wait_periods(2);
        $display("  -> 波形整体翻转, 每个桥臂内仍然互斥(不会直通)");
        axi_write(A_CTRL, 32'h0000_0001);            // 关掉反相
        wait_clk(200);

        //--------------------------------------------------------------
        // P8 PERIOD=0
        //--------------------------------------------------------------
        phase("P8  PERIOD=0 : period invalid, all 20 PWM outputs LOW");
        axi_write(A_CTRL,   32'h0000_0001);          // GLB_EN=1
        axi_write(A_PERIOD, 32'd0);
        wait_clk(500);
        $display("  -> 计数器停在 0, pwm_out 全 0");

        //--------------------------------------------------------------
        // P9 过流保护 : 5 路低有效输入, 组合关断 (不经过时钟/寄存器/AXI)
        //--------------------------------------------------------------
        phase("P9  overcurrent : oc_n[0] low -> ONLY bridge0 stops, in ~2 ns");
        axi_write(A_PERIOD, PERIOD[31:0]);
        axi_write(A_PWM_EN, 32'h0000_03FF);
        axi_write(A_CTRL,   32'h0000_0000);          // 先清故障锁存
        axi_write(A_CTRL,   32'h0000_0001);          // GLB_EN 0->1
        oc_n = 5'h1F;
        wait_periods(2);
        $display("  -> 故障前:  pwm_out = %b", pwm_out);
        $display("              gate_en = %b", gate_en);

        @(negedge aclk);
        oc_n = 5'b11110;                             // 桥 0 过流, 在时钟边沿之间拉低
        $display("  -> oc_n = %b 拉低 @ %0t  (在两个时钟上升沿之间)", oc_n, $time);
        #1;                                          // 下一个上升沿还在 19 ns 之后
        $display("  -> 仅 1 ns 后: pwm_out = %b", pwm_out);
        $display("                   ^^^^                                      ^^^^^^^^^^^^");
        $display("                   桥0 [3:0] 已到关断电平    桥1~4 [19:4] 一位都没变");
        $display("               gate_en = %b", gate_en);
        $display("                          ^^         ^^^^^^^^");
        $display("                          桥0 驱动禁用  桥1~4 驱动照常");
        wait_periods(2);
        $display("  -> STATUS: OC_NOW=%b OC_LATCHED=%b OC_SRC=%b",
                 dut.oc_now, dut.oc_latched, dut.oc_src);

        oc_n = 5'h1F;
        wait_periods(1);
        $display("  -> 输入松开后桥0 仍保持关断(锁存), 桥1~4 一直在跑");

        axi_write(A_CTRL, 32'h0000_0000);            // GLB_EN=0
        axi_write(A_CTRL, 32'h0000_0001);            // GLB_EN 0->1 = 软件确认
        wait_periods(2);
        $display("  -> 软件写 GLB_EN 0->1 确认后, 桥0 恢复输出");

        //--------------------------------------------------------------
        // P10 相位偏移 / 交错: 5 个桥错开 72 度
        //--------------------------------------------------------------
        phase("P10 phase offset : bridges interleaved by 0/72/144/216/288 deg");
        axi_write(A_PERIOD, PERIOD[31:0]);
        axi_write(A_PWM_EN, 32'h0000_03FF);
        axi_write(A_CTRL,   32'h0000_0000);          // 清故障锁存
        axi_write(A_CTRL,   32'h0000_0001);
        oc_n = 5'h1F;
        for (i = 0; i < 5; i = i + 1) cfg_bridge(i, pct2duty(50), DEAD, DEAD);
        axi_write(A_PHASE0, 32'd0);
        axi_write(A_PHASE1, PERIOD/5);
        axi_write(A_PHASE2, (PERIOD*2)/5);
        axi_write(A_PHASE3, (PERIOD*3)/5);
        axi_write(A_PHASE4, (PERIOD*4)/5);
        wait_periods(2);
        $display("  -> PHASE = 0 / %0d / %0d / %0d / %0d 拍 (共 %0d 拍一个周期)",
                 PERIOD/5, (PERIOD*2)/5, (PERIOD*3)/5, (PERIOD*4)/5, PERIOD);
        $display("  -> 5 个桥占空比都是 50%%, 但上升沿均匀错开 -> 母线电流纹波被摊平");
        $display("  -> 关键: 桥臂内 AH/AL 仍然互补、死区仍是 100 ns, AH 与 BL 仍然同相");
        $display("     (相位只是把整条波形平移, 互补关系与相位无关)");
        // 恢复同相, 便于和前面各段对比
        for (i = 0; i < 5; i = i + 1) axi_write(A_PHASE0 + 7'd4*i, 32'd0);
        wait_periods(1);

        //--------------------------------------------------------------
        // P11 涡流制动: 线圈是感性负载, 要的是平滑单向电流
        //
        //   对角导通是为电机设计的, 线圈用不了: 对角导通下线圈两端在
        //   +Vbus/-Vbus 之间来回, 纹波大 -> 制动力的纹波 -> 抖动和噪声。
        //   "静态腿 + 单极性斩波"下线圈在 +Vbus / 0V 之间, 纹波小得多,
        //   而且只有一臂在开关。
        //
        //   为让波形干净, 这一段只开桥 0 的两个半桥(其余 4 桥输出恒低),
        //   所以 ah/al/bh/bl 和 i_coil 就是"一个 H 桥驱动一个线圈"的完整画面。
        //   线圈模型参数见文件上方的 localparam。
        //--------------------------------------------------------------
        phase("P11 eddy current brake : DC / slow-decay chop / fast release / short");
        axi_write(A_PERIOD, PERIOD[31:0]);           // 4 kHz, 周期 250 us
        axi_write(A_CTRL,   32'h0000_0000);
        axi_write(A_CTRL,   32'h0000_0001);          // GLB_EN 0->1 (同时清故障锁存)
        oc_n = 5'h1F;
        for (i = 0; i < 5; i = i + 1) axi_write(A_PHASE0 + 7'd4*i, 32'd0);
        axi_write(A_DUTY0, 32'd0);
        axi_write(A_DEAD0, 32'd5);
        i_peak = 0.0;

        // (a) 纯直流: DUTY = PERIOD, A 臂死区 0, B 臂静态低
        //     AH 和 BL 整周期全通 -> 线圈吃恒定 +Vbus, 一个开关动作都没有。
        //     从 0 起流, 电流按 L/R 时间常数上升 (tau = COIL_L/COIL_R)。
        phase("P11a pure DC : DUTY=PERIOD, DEAD_A=0, mode=1  (zero switching)");
        coil_state(PERIOD, 0, 1);
        wait_periods(4);
        $display("  -> 引脚 ah=%b al=%b bh=%b bl=%b   线圈电压 = %0.1f V",
                 ah, al, bh, bl, v_coil);
        $display("  -> 线圈电流 = %0.3f A   (稳态应趋近 Vbus/R = %0.1f A)",
                 i_coil, VBUS/COIL_R);
        i_peak = i_coil;

        // (b) 慢衰减斩波: DUTY = 25%, A 臂死区 100 ns, B 臂静态低
        //     AH 斩波; AH 关断时 AL 导通, 电流经两个下管续流 -> 线圈看到 0V,
        //     所以电流下降得很慢 —— 这就是低纹波的保持电流。
        phase("P11b slow-decay chop : DUTY=25 %, mode=1  (freewheel at 0 V)");
        coil_state(pct2duty(25), DEAD, 1);
        wait_periods(4);
        $display("  -> 引脚 ah=%b al=%b bh=%b bl=%b   (BL 恒为 1, 它不开关)",
                 ah, al, bh, bl);
        $display("  -> 线圈电流 = %0.3f A  (稳态趋近 0.25*Vbus/R = %0.1f A)",
                 i_coil, 0.25*VBUS/COIL_R);

        // (c) 主动短路 (慢衰减): 两个桥臂都静态低 -> 线圈两端都到 GND
        //     电流只在两个下管里环流, 线圈只看到自己的 I*R -> 衰减很慢。
        //     这是"软释放", 靠 L/R 时间常数, 不快但也不产生反向磁场。
        phase("P11c active short : slow decay, the coil only sees its own I*R");
        coil_state(pct2duty(50), DEAD, 1);           // 先把电流建到 50% 档
        wait_periods(3);
        i_peak = i_coil;
        $display("  -> 短路前 电流 = %0.3f A", i_peak);
        coil_state(32'd0, 0, 1);                     // AL + BL 全通 = 短路
        k = 0;
        while (i_coil > 0.5 && k < 400000) begin @(negedge aclk); k = k + 1; end
        $display("  -> 引脚 ah=%b al=%b bh=%b bl=%b  (AL 与 BL 同时导通)",
                 ah, al, bh, bl);
        $display("  -> 从 %0.3f A 衰到 0.5 A 用了 %0.1f us  (慢衰减, 只靠 I*R)",
                 i_peak, k * 20.0 / 1000.0);

        // (d) 反向快释放 (快衰减): DUTY = 0, A 臂死区 0, B 臂静态高
        //     AL 与 BH 全通 -> 线圈吃 -Vbus, 电流被强行压下去。
        //     !! 关键: -Vbus 不能一直加 !!  电流一到 0 就必须切走, 否则它会
        //        继续反向充到 -Vbus/R, 白白发热而且产生反向磁场。
        //        真实控制器在过零那一刻切掉 (这里切到主动短路把电流摁在 0)。
        phase("P11d fast release : -Vbus until the current reaches 0, then short");
        coil_state(pct2duty(50), DEAD, 1);
        wait_periods(3);
        i_peak = i_coil;
        $display("  -> 释放前 电流 = %0.3f A", i_peak);
        coil_state(32'd0, 0, 2);                     // AL + BH = -Vbus
        $display("  -> 加 -Vbus, 引脚 ah=%b al=%b bh=%b bl=%b, 线圈电压 %0.1f V",
                 ah, al, bh, bl, v_coil);
        k = 0;
        while (i_coil > 0.0 && k < 400000) begin @(negedge aclk); k = k + 1; end
        t_fast_us = k * 20.0 / 1000.0;
        coil_state(32'd0, 0, 1);                     // 过零, 立刻切到短路保持
        $display("  -> 从 %0.3f A 到 0 只用了 %0.1f us  (快衰减) 后已切短路",
                 i_peak, t_fast_us);
        wait_periods(1);
        $display("  -> 切走后 电流 = %0.3f A, 没有反向充上去", i_coil);

        // (e) 滑行: 全关, 电流走体二极管回灌母线 -> 同样是快衰减
        phase("P11e coast : all off, body diodes dump the energy back to Vbus");
        coil_state(pct2duty(50), DEAD, 1);
        wait_periods(3);
        i_peak = i_coil;
        axi_write(A_CTRL, 32'h0000_0000);            // GLB_EN=0 -> 全关
        k = 0;
        while (i_coil > 0.0 && k < 400000) begin @(negedge aclk); k = k + 1; end
        $display("  -> 从 %0.3f A 滑行到 0 用了 %0.1f us  (对比反向电压的 %0.1f us)",
                 i_peak, k * 20.0 / 1000.0, t_fast_us);
        $display("  -> 滑行也是快的: 体二极管把线圈端电压钳到母线, 能量回灌供电");
        $display("     (只有主动短路是慢的, 因为那个 0V 是开关给的, 不是二极管给的)");

        axi_write(A_CTRL, 32'h0000_0001);
        wait_periods(1);

        // 收尾前把桥恢复成普通对角导通, 不影响前面的段
        axi_write(A_PWM_EN, 32'h0000_03FF);
        axi_write(A_CTRL,   32'h0000_0001);
        wait_periods(1);

        //--------------------------------------------------------------
        // 收尾
        //--------------------------------------------------------------
        phase("DONE");
        axi_write(A_PERIOD, PERIOD[31:0]);
        wait_clk(200);
        axi_read(A_ID, rd);
        $display("  ID register = 0x%08X  (expect 0x50574D31)", rd);

        $display("");
        $display("########################################################");
        $display("#  仿真结束  总时长 %0t", $time);
        $display("#  波形里值得看的几处 (4 kHz / 死区 100 ns):");
        $display("#    P1 : 5 个 H 桥, 20 路 PWM, 占空比 10/30/50/70/90%%");
        $display("#    P2 : 桥0 占空比 10%% -> 100%% 每档一个周期, 单调递增");
        $display("#    P3 : 占空比 100%%, 上管除死区外整周期导通");
        $display("#    P4 : A 臂死区 5 拍 / B 臂死区 15 拍 (缩到 1 us 对比)");
        $display("#    P5 : 只有 AH/AL 变低, BH/BL 照常");
        $display("#    P6 : 20 路全低, gate_en 不变");
        $display("#    P7 : 电平整体翻转");
        $display("#    P9 : oc_n 拉低 -> 1 ns 内**只有该桥**关断, 其余 4 桥照常跑");
        $display("#    P10: 5 个桥相位错开 72 度 (交错), 桥臂内互补与死区不变");
        $display("#    P11: 涡流制动 - 纯直流 / 慢衰减斩波 / 反向快释放 / 主动短路 / 滑行");
        $display("#         看 ah/al/bh/bl 四个引脚 + i_coil (线圈电流, 由 L-R 模型算出)");
        $display("#         建议加: /tb_pwm/i_coil  (radix 选 analog 或 decimal)");
        $display("#");
        $display("#  缩放提示: 300 us 看占空比, 1 us 看死区, 二者不能同屏");
        $display("########################################################");
        $finish;
    end

    // 看门狗 (整个激励约 5.3 ms)
    initial begin
        #20_000_000;
        $display(" FATAL: simulation watchdog timeout");
        $finish;
    end

endmodule
