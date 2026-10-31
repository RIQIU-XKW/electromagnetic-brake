
`timescale 1 ns / 1 ps

//=============================================================================
// pwm_axi_ip : AXI4-Lite 5 x H-bridge complementary PWM generator
//-----------------------------------------------------------------------------
//  5 complete H bridges, each built from the reusable pwm_h_bridge module
//  (which is in turn built from two pwm_half_bridge legs).  20 PWM outputs and
//  10 gate-driver enables in total.
//
//  ONE H BRIDGE
//  ------------
//        +Vbus
//          |            |
//        [AH]         [BH]          leg A = AH / AL   (complementary pair)
//          |            |           leg B = BH / BL   (complementary pair)
//          +--- MOTOR ---+
//          |            |
//        [AL]         [BL]
//          |            |
//         GND          GND
//
//  Drive mode: lock anti-phase (bipolar).  The DIAGONAL switches conduct
//  together, so AH is IN PHASE with BL and AL is in phase with BH.
//
//  OUTPUT INDEX MAPPING  (bridge i = 0..4)
//  ---------------------------------------
//     pwm_out[4*i + 0] = AH   leg A high side
//     pwm_out[4*i + 1] = AL   leg A low  side
//     pwm_out[4*i + 2] = BH   leg B high side
//     pwm_out[4*i + 3] = BL   leg B low  side
//
//     gate_en[2*i + 0] = leg A gate-driver enable  (EM pin, ACTIVE LOW:
//     gate_en[2*i + 1] = leg B gate-driver enable   引脚低 = 开启输出, 高 = 关断)
//     PWM_EN bit = 1 表示"使能", 引脚上取反后输出; 复位/过流时引脚为高(关断)。
//
//  REGISTER MAP (offsets from C_S00_AXI_BASEADDR, 64-byte window)
//  ---------------------------------------------------------------
//     0x00 CTRL      RW  [0] GLB_EN   global modulation enable (1 = run)
//                        [1] INV_PWM  invert the level of all 20 PWM outputs
//                        [31:2] reserved
//     0x04 PERIOD    RW  PWM period in s00_axi_aclk cycles, 1..0xFFFFFFFF.
//                        0 = no valid period -> all PWM outputs forced low.
//     0x08 DUTY0     RW  bridge 0 compare value
//     0x0C DUTY1     RW  bridge 1 compare value
//     0x10 DUTY2     RW  bridge 2 compare value
//     0x14 DUTY3     RW  bridge 3 compare value
//     0x18 DUTY4     RW  bridge 4 compare value
//     0x1C DEAD0     RW  bridge 0 dead time, {leg B[31:16], leg A[15:0]}
//     0x20 DEAD1     RW  bridge 1 dead time, {leg B       , leg A       }
//     0x24 DEAD2     RW  bridge 2 dead time, {leg B       , leg A       }
//     0x28 DEAD3     RW  bridge 3 dead time, {leg B       , leg A       }
//     0x2C DEAD4     RW  bridge 4 dead time, {leg B       , leg A       }
//     0x30 PWM_EN    RW  [9:0] 10 half-bridge gate-driver enables
//                             bit 2*i = bridge i leg A, bit 2*i+1 = bridge i leg B
//     0x34 SCRATCH   RW  scratch register (AXI read/write test)
//     0x38 ID        RO  0x50574D31 ("PWM1")
//     0x3C STATUS    RO  [0]     GLB_EN     [1]  PERIOD_OK
//                        [2]     CNT_ZERO   [3]  ANY_EN
//                        [4]     OC_NOW     (an oc_n input is low right now)
//                        [5]     OC_LATCHED (an overcurrent happened; sticky)
//                        [12:8]  OC_SRC     (which bridge(s) tripped; sticky)
//                        [15:13] 0          [31:16] pwm_cnt[15:0]
//
//  OVERCURRENT PROTECTION
//  ----------------------
//  Five active-low overcurrent inputs oc_n[4:0], one per H bridge, with
//  internal pull-ups.  One LUT of combinational logic sits between the input
//  pin and the PWM outputs of THAT bridge, so a fault turns its four switches
//  off in ~2 ns - far faster than any register write or interrupt could.
//  ONLY the faulted bridge stops; the other four keep switching.
//  A synchronised sticky copy is latched into STATUS[5] and STATUS[12:8];
//  software acknowledges by writing CTRL.GLB_EN = 1 (a deliberate 0 -> 1
//  edge).  No new registers were added for this.
//
//  WAVEFORM OF ONE BRIDGE (counter shared by all 5 bridges, so they stay
//  phase locked; only duty and dead times differ)
//
//     cnt     0 ... DTA-1 | DTA ... D-1 | D ... D+DTA-1 | D+DTA ... PERIOD-1
//     AH            0     |      1      |       0       |       0
//     AL            0     |      0      |       0       |       1
//     BL            0     |      1      |       0       |       0
//     BH            0     |      0      |       0       |       1
//
//  (leg B uses DTB; with DTA = DTB, AH and BL are exactly edge aligned)
//
//  All 20 outputs are registered inside the half bridges, so they are glitch
//  free and lag the counter by exactly one aclk cycle.
//
//  SAFETY: shoot-through is impossible by construction - see pwm_half_bridge.
//=============================================================================

	module pwm_axi_ip #
	(
		// Users to add parameters here

		// User parameters ends
		// Do not modify the parameters beyond this line


		// Parameters of Axi Slave Bus Interface S00_AXI
		parameter integer C_S00_AXI_DATA_WIDTH	= 32,
		parameter integer C_S00_AXI_ADDR_WIDTH	= 7
	)
	(
		// Users to add ports here
		// 20 PWM outputs.  Bridge i (0..4):
		//   [4*i+0] = AH  leg A high side      [4*i+1] = AL  leg A low side
		//   [4*i+2] = BH  leg B high side      [4*i+3] = BL  leg B low  side
		output wire [19:0] pwm_out,
		// 10 gate-driver enables, one per half bridge:
		//   [2*i+0] = bridge i leg A          [2*i+1] = bridge i leg B
		output wire [9:0]  gate_en,
		// Overcurrent protection inputs, one per H bridge, ACTIVE LOW:
		//   oc_n[i] == 0  ->  bridge i reports overcurrent
		// 0 is the asserted (fault) level on purpose: the pin is given an
		// internal pull-up, so an UNCONNECTED or healthy input reads as 1 and
		// a broken wire can never mask a real fault nor cause a false trip.
		// Wire the open-drain output of a shunt comparator, or the fault
		// output of the gate driver, directly to these pins.
		input  wire [4:0]  oc_n,
		// User ports ends
		// Do not modify the ports beyond this line


		// Ports of Axi Slave Bus Interface S00_AXI
		input wire  s00_axi_aclk,
		input wire  s00_axi_aresetn,
		input wire [C_S00_AXI_ADDR_WIDTH-1 : 0] s00_axi_awaddr,
		input wire [2 : 0] s00_axi_awprot,
		input wire  s00_axi_awvalid,
		output wire  s00_axi_awready,
		input wire [C_S00_AXI_DATA_WIDTH-1 : 0] s00_axi_wdata,
		input wire [(C_S00_AXI_DATA_WIDTH/8)-1 : 0] s00_axi_wstrb,
		input wire  s00_axi_wvalid,
		output wire  s00_axi_wready,
		output wire [1 : 0] s00_axi_bresp,
		output wire  s00_axi_bvalid,
		input wire  s00_axi_bready,
		input wire [C_S00_AXI_ADDR_WIDTH-1 : 0] s00_axi_araddr,
		input wire [2 : 0] s00_axi_arprot,
		input wire  s00_axi_arvalid,
		output wire  s00_axi_arready,
		output wire [C_S00_AXI_DATA_WIDTH-1 : 0] s00_axi_rdata,
		output wire [1 : 0] s00_axi_rresp,
		output wire  s00_axi_rvalid,
		input wire  s00_axi_rready
	);

	//------------------------------------------------------------------
	// Register values coming back from the AXI4-Lite slave
	//------------------------------------------------------------------
	wire [31:0] ctrl_reg;
	wire [31:0] period_reg;
	wire [31:0] duty0_reg, duty1_reg, duty2_reg, duty3_reg, duty4_reg;
	wire [31:0] dead0_reg, dead1_reg, dead2_reg, dead3_reg, dead4_reg;
	wire [9:0]  pwm_en_reg;
	wire [9:0]  pwm_mode_reg;	// PWM_EN[15:10]: per-bridge leg B mode
	wire [31:0] phase0_reg, phase1_reg, phase2_reg, phase3_reg, phase4_reg;
	wire [31:0] status_word;

	// Per-bridge arrays gathered from the individual register outputs
	wire [31:0] duty [0:4];
	wire [15:0] dt_a [0:4];		// leg A dead time of each bridge
	wire [15:0] dt_b [0:4];		// leg B dead time of each bridge
	wire [31:0] phase [0:4];	// phase offset of each bridge, in aclk cycles

	// Raw switching outputs of the 5 bridges, BEFORE the overcurrent mask.
	// pwm_out / gate_en are these two signals masked by the protection logic.
	wire [19:0] pwm_raw;
	wire [9:0]  gate_en_raw;

// Instantiation of Axi Bus Interface S00_AXI
	pwm_axi_ip_slave_lite_v1_0_S00_AXI # ( 
		.C_S_AXI_DATA_WIDTH(C_S00_AXI_DATA_WIDTH),
		.C_S_AXI_ADDR_WIDTH(C_S00_AXI_ADDR_WIDTH)
	) pwm_axi_ip_slave_lite_v1_0_S00_AXI_inst (
		.ctrl_reg(ctrl_reg),
		.period_reg(period_reg),
		.duty0_reg(duty0_reg),
		.duty1_reg(duty1_reg),
		.duty2_reg(duty2_reg),
		.duty3_reg(duty3_reg),
		.duty4_reg(duty4_reg),
		.dead0_reg(dead0_reg),
		.dead1_reg(dead1_reg),
		.dead2_reg(dead2_reg),
		.dead3_reg(dead3_reg),
		.dead4_reg(dead4_reg),
		.pwm_en_reg(pwm_en_reg),
		.pwm_mode_reg(pwm_mode_reg),
		.phase0_reg(phase0_reg),
		.phase1_reg(phase1_reg),
		.phase2_reg(phase2_reg),
		.phase3_reg(phase3_reg),
		.phase4_reg(phase4_reg),
		.status_word(status_word),
		.S_AXI_ACLK(s00_axi_aclk),
		.S_AXI_ARESETN(s00_axi_aresetn),
		.S_AXI_AWADDR(s00_axi_awaddr),
		.S_AXI_AWPROT(s00_axi_awprot),
		.S_AXI_AWVALID(s00_axi_awvalid),
		.S_AXI_AWREADY(s00_axi_awready),
		.S_AXI_WDATA(s00_axi_wdata),
		.S_AXI_WSTRB(s00_axi_wstrb),
		.S_AXI_WVALID(s00_axi_wvalid),
		.S_AXI_WREADY(s00_axi_wready),
		.S_AXI_BRESP(s00_axi_bresp),
		.S_AXI_BVALID(s00_axi_bvalid),
		.S_AXI_BREADY(s00_axi_bready),
		.S_AXI_ARADDR(s00_axi_araddr),
		.S_AXI_ARPROT(s00_axi_arprot),
		.S_AXI_ARVALID(s00_axi_arvalid),
		.S_AXI_ARREADY(s00_axi_arready),
		.S_AXI_RDATA(s00_axi_rdata),
		.S_AXI_RRESP(s00_axi_rresp),
		.S_AXI_RVALID(s00_axi_rvalid),
		.S_AXI_RREADY(s00_axi_rready)
	);

	// Add user logic here

	//------------------------------------------------------------------
	// Control / configuration
	//------------------------------------------------------------------
	wire        glb_en    = ctrl_reg[0];	// global modulation enable
	wire        inv_pwm   = ctrl_reg[1];	// invert all PWM output levels

	wire        period_ok = (period_reg != 32'd0);
	wire [31:0] period_end = period_reg - 32'd1;	// last count of the period

	// Both the global enable and a valid period are required before any switch
	// is driven.  This single signal is fed to every half bridge.
	wire        run = glb_en && period_ok;

	assign duty[0] = duty0_reg;
	assign duty[1] = duty1_reg;
	assign duty[2] = duty2_reg;
	assign duty[3] = duty3_reg;
	assign duty[4] = duty4_reg;

	// Each DEADx register carries both leg dead times of one bridge
	assign dt_a[0] = dead0_reg[15:0];	assign dt_b[0] = dead0_reg[31:16];
	assign dt_a[1] = dead1_reg[15:0];	assign dt_b[1] = dead1_reg[31:16];
	assign dt_a[2] = dead2_reg[15:0];	assign dt_b[2] = dead2_reg[31:16];
	assign dt_a[3] = dead3_reg[15:0];	assign dt_b[3] = dead3_reg[31:16];
	assign dt_a[4] = dead4_reg[15:0];	assign dt_b[4] = dead4_reg[31:16];

	// Phase offset of each bridge.  PHASEn must stay in [0, PERIOD).
	assign phase[0] = phase0_reg;
	assign phase[1] = phase1_reg;
	assign phase[2] = phase2_reg;
	assign phase[3] = phase3_reg;
	assign phase[4] = phase4_reg;

	//------------------------------------------------------------------
	// Free-running period counter.  It is generated ONCE here and shared by
	// all 5 bridges, which is what keeps the 20 outputs phase locked.  Only
	// the duty and the dead times differ per bridge.
	//------------------------------------------------------------------
	reg [31:0] pwm_cnt;

	always @(posedge s00_axi_aclk)
	begin
	  if (s00_axi_aresetn == 1'b0)
	    pwm_cnt <= 32'd0;
	  else if (period_ok == 1'b0)
	    pwm_cnt <= 32'd0;			// no valid period: park the counter
	  else if (pwm_cnt >= period_end)
	    pwm_cnt <= 32'd0;
	  else
	    pwm_cnt <= pwm_cnt + 32'd1;
	end

	//------------------------------------------------------------------
	// 5 x H bridge.  pwm_h_bridge contains two pwm_half_bridge legs; leg B's
	// outputs are cross-connected inside that module so that AH is in phase
	// with BL (the forward diagonal of the bridge).
	//------------------------------------------------------------------
	genvar gi;

	generate
	  for (gi = 0; gi < 5; gi = gi + 1) begin : g_bridge
	    //--------------------------------------------------------------
	    // Per-bridge phase shift.
	    //
	    // Every bridge counts on the SAME free-running pwm_cnt, but each one
	    // compares against its own shifted value, so PHASEn delays bridge n by
	    // PHASEn aclk cycles.  PHASEn = 0 keeps the bridge exactly where it was.
	    //
	    // Written as period_reg - (phase - pwm_cnt) rather than the more
	    // obvious pwm_cnt + period_reg - phase so that no 32 bit add can
	    // overflow when PERIOD is close to 0xFFFFFFFF: in this branch
	    // pwm_cnt < phase, so (phase - pwm_cnt) is in [1, PERIOD] and the
	    // subtraction lands in [0, PERIOD-1].
	    //
	    // Both legs of a bridge use the SAME cnt_i, so the "AH in phase with
	    // BL" property is completely unaffected by the phase setting.
	    //--------------------------------------------------------------
	    wire [31:0] cnt_i = (period_ok == 1'b0) ? 32'd0
	                      : (pwm_cnt >= phase[gi]) ? (pwm_cnt - phase[gi])
	                                               : (period_reg - (phase[gi] - pwm_cnt));

	    pwm_h_bridge u_h_bridge
	    (
		.clk		(s00_axi_aclk),
		.rstn		(s00_axi_aresetn),
		.run		(run),
		.inv		(inv_pwm),
		.en		(pwm_en_reg[2*gi +: 2]),
		.cnt		(cnt_i),
		.duty		(duty[gi]),
		.dead_a		(dt_a[gi]),
		.dead_b		(dt_b[gi]),
		.mode_b		(pwm_mode_reg[2*gi +: 2]),
		.ah		(pwm_raw[4*gi + 0]),
		.al		(pwm_raw[4*gi + 1]),
		.bh		(pwm_raw[4*gi + 2]),
		.bl		(pwm_raw[4*gi + 3]),
		.en_a		(gate_en_raw[2*gi + 0]),
		.en_b		(gate_en_raw[2*gi + 1])
	    );
	  end
	endgenerate

	//------------------------------------------------------------------
	// OVERCURRENT PROTECTION
	//------------------------------------------------------------------
	//  oc_n[4:0] : one active-low overcurrent input per H bridge, with an
	//              internal pull-up (see the port comment).  Reading 1 means
	//              "healthy"; 0 means "bridge i is overcurrent".
	//
	//  TWO PATHS, and the difference matters:
	//
	//  1. FAST ASYNCHRONOUS SHUTDOWN (the one that protects the FETs)
	//     oc_now is combined with the latched flag and applied STRAIGHT to the
	//     output pins, combinationally.  No clock, no register write, no AXI
	//     transaction - so the switches are off after one LUT delay (~2 ns)
	//     instead of after the 20 ns clock edge plus the microseconds a
	//     software reaction would take.  A MOSFET dies long before software
	//     could ever react.
	//
	//  2. SYNCHRONISED STICKY FLAG (the one software reads)
	//     oc_n is passed through two flip flops before being latched, so a
	//     metastable input can never corrupt the status register.  The flag
	//     stays set until software acknowledges it.
	//
	//  The shutdown forces the switches OFF, which depends on the polarity:
	//     INV_PWM = 0 (active high driver) -> drive the pins LOW
	//     INV_PWM = 1 (active low  driver) -> drive the pins HIGH
	//  Driving all pins low on an active-low driver would turn EVERY switch
	//  ON, so the mask is {20{inv_pwm}} and not a plain zero.
	//------------------------------------------------------------------

	// live, asynchronous: any input low = fault
	wire        oc_now = ~(&oc_n);

	// synchronised copy for the sticky logic
	reg  [4:0]  oc_n_s1;
	reg  [4:0]  oc_n_s2;

	reg         oc_latched;		// "an overcurrent has happened"
	reg  [4:0]  oc_src;		// which bridges have tripped
	reg         glb_en_d;		// for the GLB_EN rising edge

	always @(posedge s00_axi_aclk)
	begin
	  if (s00_axi_aresetn == 1'b0) begin
	    oc_n_s1    <= 5'h1F;
	    oc_n_s2    <= 5'h1F;
	    oc_latched <= 1'b0;
	    oc_src     <= 5'd0;
	    glb_en_d   <= 1'b0;
	  end
	  else begin
	    oc_n_s1  <= oc_n;
	    oc_n_s2  <= oc_n_s1;
	    glb_en_d <= glb_en;

	    if (glb_en & ~glb_en_d) begin
	      // Software wrote CTRL.GLB_EN = 1 (a 0 -> 1 edge).  This is the
	      // acknowledge: it clears the latched fault.  Requiring a deliberate
	      // re-enable means the IP can never silently restart into a fault.
	      oc_latched <= 1'b0;
	      oc_src     <= 5'd0;
	    end
	    else if (~(&oc_n_s2)) begin
	      oc_latched <= 1'b1;
	      oc_src     <= oc_src | ~oc_n_s2;
	    end
	  end
	end

	//------------------------------------------------------------------
	// PER-BRIDGE fast output mask
	//------------------------------------------------------------------
	//  ONLY the bridge that reports the fault is shut down - the other four
	//  keep switching normally.  A bridge is held off while its own oc_n input
	//  is low, and stays off once it has tripped until software acknowledges.
	//
	//  If you would rather have ANY fault stop ALL five bridges, replace the
	//  next line with a broadcast:
	//      wire [4:0] oc_br_off = {5{|oc_br_off_raw}};
	//------------------------------------------------------------------
	wire [4:0] oc_br_off = ~oc_n | oc_src;

	genvar gj;

	generate
	  for (gj = 0; gj < 5; gj = gj + 1) begin : g_oc_mask
	    // NOTE the OFF level depends on the driver polarity: on an active-low
	    // driver (INV_PWM=1) driving the pins LOW would turn the switches ON.
	    assign pwm_out[4*gj + 0] = oc_br_off[gj] ? inv_pwm : pwm_raw[4*gj + 0];
	    assign pwm_out[4*gj + 1] = oc_br_off[gj] ? inv_pwm : pwm_raw[4*gj + 1];
	    assign pwm_out[4*gj + 2] = oc_br_off[gj] ? inv_pwm : pwm_raw[4*gj + 2];
	    assign pwm_out[4*gj + 3] = oc_br_off[gj] ? inv_pwm : pwm_raw[4*gj + 3];
	    // Gate-driver enable pins (EM) are ACTIVE LOW on this board:
	    //   pin LOW = driver enabled / output ON, pin HIGH = output OFF.
	    // The register bit keeps its friendly meaning (1 = enable) and the pin is
	    // its inverse, so reset (bit 0) and an overcurrent both drive the pin HIGH
	    // = outputs off.  (2026-10-30: before this the pin was driven directly,
	    // which made "1 = enable" switch the outputs OFF and made the overcurrent
	    // mask drive the enable pin LOW = ON.)
	    assign gate_en[2*gj + 0] = oc_br_off[gj] ? 1'b1    : ~gate_en_raw[2*gj + 0];
	    assign gate_en[2*gj + 1] = oc_br_off[gj] ? 1'b1    : ~gate_en_raw[2*gj + 1];
	  end
	endgenerate

	//------------------------------------------------------------------
	// Read-only STATUS word (0x3C)
	//------------------------------------------------------------------
	assign status_word = { pwm_cnt[15:0],		// [31:16] live counter snapshot
	                       3'd0,			// [15:13] reserved
	                       oc_src,			// [12:8]  bridges that tripped (sticky)
	                       2'd0,			// [7:6]   reserved
	                       oc_latched,		// [5]     overcurrent happened (sticky,
	                       oc_now,			//         cleared by CTRL.GLB_EN 0->1)
	                       (|pwm_en_reg),		// [3]     any half bridge enabled
	                       (pwm_cnt == 32'd0),	// [2]     counter at period start
	                       period_ok,		// [1]     PERIOD register != 0
	                       glb_en };		// [0]     global enable mirror

	// User logic ends

	endmodule
