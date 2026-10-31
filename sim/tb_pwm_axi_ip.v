//=============================================================================
// tb_pwm_axi_ip.v : self-checking testbench for the pwm_axi_ip AXI4-Lite
//                   5 x H-bridge complementary PWM generator
//-----------------------------------------------------------------------------
// Topology under test
//   5 H bridges.  Bridge i drives 4 switches:
//     pwm_out[4*i+0] = AH  leg A high side   pwm_out[4*i+1] = AL  leg A low side
//     pwm_out[4*i+2] = BH  leg B high side   pwm_out[4*i+3] = BL  leg B low  side
//   (AH,AL) is one complementary pair, (BH,BL) is the other, so 5 bridges give
//   10 complementary pairs = 20 PWM outputs.
//   The bridge runs in lock anti-phase mode, so the DIAGONAL switches conduct
//   together:  AH is in phase with BL  and  AL is in phase with BH.
//   gate_en[2*i+0] = leg A gate enable, gate_en[2*i+1] = leg B gate enable.
//
// Reference waveform for bridge i (counter cnt = 0 .. PERIOD-1, shared by all
// bridges so they stay phase locked), DUTY = D, leg dead times DTA / DTB:
//     AH on for cnt in [DTA, D)          AL on for cnt in [D + DTA, PERIOD)
//     BL on for cnt in [DTB, D)          BH on for cnt in [D + DTB, PERIOD)
//
// All 20 outputs are registered inside the half bridges, so the pins lag the
// counter by exactly one clock: at the clock where cnt == k the pins carry
// f(k-1).  The model below accounts for that explicitly.
//
// Checks performed
//   T1  reset: all 20 PWM outputs low, all 10 gate enables low
//   T2  ID register (0x38) reads 0x50574D31
//   T3  every RW register writes and reads back (incl. packed DEADx)
//   T4  write strobes (byte enables) are honoured
//   T5  read-only registers ignore writes; STATUS exposes a live counter
//   T6  bridge 0 leg A measured: period, high = D-DTA, both gaps = DTA
//   T7  bridge 0 leg B: high = D-DTB, gaps = DTB (independent of leg A)
//   T8  all 5 bridges carry their own duty and their own two dead times
//   T9  AH is in phase with BL        (the forward diagonal)
//   T10 AL is in phase with BH        (the reverse diagonal)
//   T11 gate_en matches the PWM_EN bit mapping
//   T12 clearing one leg enable only affects that leg's two pins
//   T13 GLB_EN=0 forces all 20 pins low, gate_en unchanged
//   T14 PERIOD=0 forces all 20 pins low
//   T15 INV_PWM is the exact bitwise complement of all 20 pins
//   T16 whole period compared sample by sample against the model
//   T17 randomised register sweep with the continuous safety monitor running
//   T18 the real operating point: 4 kHz, 100 ns dead time, duty 10..100 %
//   T19 overcurrent protection: combinational shutdown, sticky status,
//       acknowledge via a CTRL.GLB_EN 0->1 edge, polarity aware off level
//
// Overcurrent input
//   oc_n[4:0], active low, one per H bridge, pulled up in the IP.  A test that
//   wants no fault must keep it at 5'h1F.
//
// Continuous monitor (active for the whole run, polarity independent):
//   * inside EVERY leg the two switches must never be on in the same clock
//     (this is the real shoot-through condition for an H bridge - AH and BL
//      being on together is NOT shoot-through, it is the forward diagonal)
//   * when a leg is enabled and the core runs, its two pins must never carry
//     the "switch on" level together
//=============================================================================

`timescale 1 ns / 1 ps

module tb_pwm_axi_ip;

	//------------------------------------------------------------------
	// Clock / reset : 50 MHz, same as the PS FCLK_CLK0
	//------------------------------------------------------------------
	localparam integer CLK_HALF = 10;

	reg aclk    = 1'b0;
	reg aresetn = 1'b0;

	always #CLK_HALF aclk = ~aclk;

	//------------------------------------------------------------------
	// AXI4-Lite master signals
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
	// DUT outputs / protection input
	//------------------------------------------------------------------
	wire [19:0] pwm_out;
	wire [9:0]  gate_en;

	// Overcurrent inputs, active low; 1 = healthy.  Held high so that no test
	// trips the protection unless it means to.
	reg  [4:0]  oc_n = 5'h1F;

	// Register offsets (7 bit address bus -> 128 byte window)
	localparam [6:0] A_CTRL    = 7'h00,
	                 A_PERIOD  = 7'h04,
	                 A_DUTY0   = 7'h08,
	                 A_DUTY1   = 7'h0C,
	                 A_DUTY2   = 7'h10,
	                 A_DUTY3   = 7'h14,
	                 A_DUTY4   = 7'h18,
	                 A_DEAD0   = 7'h1C,
	                 A_DEAD1   = 7'h20,
	                 A_DEAD2   = 7'h24,
	                 A_DEAD3   = 7'h28,
	                 A_DEAD4   = 7'h2C,
	                 A_PWM_EN  = 7'h30,
	                 A_SCRATCH = 7'h34,
	                 A_ID      = 7'h38,
	                 A_STATUS  = 7'h3C,
	                 A_PHASE0  = 7'h40,
	                 A_PHASE1  = 7'h44,
	                 A_PHASE2  = 7'h48,
	                 A_PHASE3  = 7'h4C,
	                 A_PHASE4  = 7'h50;

	localparam [31:0] IP_ID_EXPECTED = 32'h50574D31;

	// switch index helpers
	function integer AH; input integer ii; begin AH = 4*ii + 0; end endfunction
	function integer AL; input integer ii; begin AL = 4*ii + 1; end endfunction
	function integer BH; input integer ii; begin BH = 4*ii + 2; end endfunction
	function integer BL; input integer ii; begin BL = 4*ii + 3; end endfunction

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
	// Bookkeeping
	//------------------------------------------------------------------
	integer pass_cnt  = 0;
	integer fail_cnt  = 0;
	integer shoot_err = 0;		// continuous shoot-through monitor hits
	integer phase_err = 0;		// reference model mismatch counter
	integer i, k, p;

	task check;
		input [8*56-1:0] name;
		input [31:0]     got;
		input [31:0]     exp;
		begin
			if (got === exp) begin
				pass_cnt = pass_cnt + 1;
				$display("  PASS  %0s : 0x%08X", name, got);
			end else begin
				fail_cnt = fail_cnt + 1;
				$display("  FAIL  %0s : got 0x%08X expected 0x%08X", name, got, exp);
			end
		end
	endtask

	task check_int;
		input [8*56-1:0] name;
		input integer    got;
		input integer    exp;
		begin
			if (got === exp) begin
				pass_cnt = pass_cnt + 1;
				$display("  PASS  %0s : %0d", name, got);
			end else begin
				fail_cnt = fail_cnt + 1;
				$display("  FAIL  %0s : got %0d expected %0d", name, got, exp);
			end
		end
	endtask

	//------------------------------------------------------------------
	// Control signals delayed by one clock.
	//
	// The 20 pins are registered inside the IP, so the levels on the pins at
	// clock n were produced from the run / enable / polarity values of clock
	// n-1.  The pin level check below must therefore use the DELAYED control
	// values, otherwise the single clock in which INV_PWM flips is checked
	// with the new polarity against pins that still carry the old polarity -
	// which looks like shoot-through during a dead time but is not.
	//------------------------------------------------------------------
	reg       run_d = 1'b0;
	reg       inv_d = 1'b0;
	reg [9:0] en_d  = 10'd0;

	always @(posedge aclk) begin
		run_d <= dut.run;
		inv_d <= dut.inv_pwm;
		en_d  <= dut.pwm_en_reg;
	end

	//------------------------------------------------------------------
	// Continuous safety monitor.  Built as a generate block so the per bridge
	// internals can be referenced with a constant index.  Sampled on the
	// falling clock edge so all values have settled.
	//------------------------------------------------------------------
	genvar gm;

	generate
	  for (gm = 0; gm < 5; gm = gm + 1) begin : g_mon
	    always @(negedge aclk) begin
	      if (aresetn === 1'b1) begin
	        // Shoot-through = both switches of one leg COMMANDED on in the same
	        // clock.  hi_raw / lo_raw are the polarity independent switch-on
	        // intent; the registered pins hi_r / lo_r are only levels, and with
	        // INV_PWM=1 a HIGH pin means the switch is OFF, so checking the pins
	        // here would raise a false alarm during every dead time.
	        if (dut.g_bridge[gm].u_h_bridge.u_leg_a.hi_raw &&
	            dut.g_bridge[gm].u_h_bridge.u_leg_a.lo_raw)
	          shoot_err = shoot_err + 1;
	        if (dut.g_bridge[gm].u_h_bridge.u_leg_b.hi_raw &&
	            dut.g_bridge[gm].u_h_bridge.u_leg_b.lo_raw)
	          shoot_err = shoot_err + 1;

	        // pin level check, polarity independent, using the DELAYED control
	        // values so it matches the cycle that generated the pins:
	        //   inv = 0 -> ON is 1, both high would be shoot-through
	        //   inv = 1 -> ON is 0, both low  would be shoot-through
	        if (run_d && en_d[2*gm]) begin
	          if (inv_d) begin
	            if (!pwm_out[4*gm+0] && !pwm_out[4*gm+1]) shoot_err = shoot_err + 1;
	          end else begin
	            if ( pwm_out[4*gm+0] &&  pwm_out[4*gm+1]) shoot_err = shoot_err + 1;
	          end
	        end
	        if (run_d && en_d[2*gm+1]) begin
	          if (inv_d) begin
	            if (!pwm_out[4*gm+2] && !pwm_out[4*gm+3]) shoot_err = shoot_err + 1;
	          end else begin
	            if ( pwm_out[4*gm+2] &&  pwm_out[4*gm+3]) shoot_err = shoot_err + 1;
	          end
	        end
	      end
	    end
	  end
	endgenerate

	//------------------------------------------------------------------
	// AXI4-Lite write.  BREADY is held high until the write response has been
	// consumed, otherwise the slave keeps BVALID asserted and the next
	// transaction's handshake gets corrupted.
	//------------------------------------------------------------------
	task axi_write;
		input [6:0]  addr;
		input [31:0] data;
		begin
			axi_write_strobe(addr, data, 4'hF);
		end
	endtask

	task axi_write_strobe;
		input [6:0]  addr;
		input [31:0] data;
		input [3:0]  strb;
		begin
			@(negedge aclk);
			while (!(awready && wready)) @(negedge aclk);
			awaddr  = addr;
			awprot  = 3'b000;
			awvalid = 1'b1;
			wdata   = data;
			wstrb   = strb;
			wvalid  = 1'b1;
			bready  = 1'b1;
			@(negedge aclk);
			awvalid = 1'b0;
			wvalid  = 1'b0;
			while (!bvalid) @(negedge aclk);
			if (bresp !== 2'b00) begin
				fail_cnt = fail_cnt + 1;
				$display("  FAIL  AXI write bresp=%b at 0x%02X", bresp, addr);
			end
			@(negedge aclk);
			bready = 1'b0;
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

	task align_period;
		integer guard;
		begin
			guard = 0;
			@(negedge aclk);
			while (dut.pwm_cnt !== 32'd0 && guard < 20000) begin
				@(negedge aclk);
				guard = guard + 1;
			end
		end
	endtask

	//------------------------------------------------------------------
	// Reference values used by the waveform checks
	//------------------------------------------------------------------
	integer   duty_m [0:4];
	integer   phase_m [0:4];		// per-bridge phase offset in aclk cycles
	integer   dta_m  [0:4];		// leg A dead time
	integer   dtb_m  [0:4];		// leg B dead time
	integer   mode_m [0:4];		// leg B mode: 0 normal, 1 static low, 2 static high
	integer   per_m;
	reg [9:0] en_m;
	reg [9:0] mode_vec;			// the five mode_m values packed
	reg       inv_m;

	reg [19:0] cap [0:1][0:1023];

	//------------------------------------------------------------------
	// Measure one complementary pair (one leg) of one bridge, sampled once
	// per clock.  Only meaningful when the leg actually toggles.
	//------------------------------------------------------------------
	task measure_leg;
		input  integer bridge;
		input  integer leg;		// 0 = A, 1 = B
		output integer t_high_hi;
		output integer gap_hl;
		output integer t_high_lo;
		output integer gap_lh;
		output integer t_period;
		integer hi_idx, lo_idx, c;
		begin
			// leg B's outputs are cross connected in the RTL: BL carries the
			// half bridge's "high" window and BH carries the "low" window
			if (leg == 0) begin hi_idx = 4*bridge+0; lo_idx = 4*bridge+1; end
			else          begin hi_idx = 4*bridge+3; lo_idx = 4*bridge+2; end

			// Find a TRUE rising edge of the high side.
			// Aligning on the counter is NOT sufficient for a phase shifted
			// bridge: at the period boundary its pin may already be high, and
			// counting from there would report only the tail of the pulse
			// (e.g. 1251 instead of 6245 for a 7500 cycle phase shift).
			c = 0;
			while (pwm_out[hi_idx] === 1'b1 && c < 20000) begin
				@(negedge aclk); c = c + 1;
			end
			c = 0;
			while (pwm_out[hi_idx] !== 1'b1 && c < 20000) begin
				@(negedge aclk); c = c + 1;
			end
			c = 0;
			while (pwm_out[hi_idx] === 1'b1 && c < 20000) begin
				@(negedge aclk); c = c + 1;
			end
			t_high_hi = c;

			c = 0;
			while (pwm_out[lo_idx] !== 1'b1 && c < 20000) begin
				@(negedge aclk); c = c + 1;
			end
			gap_hl = c;

			c = 0;
			while (pwm_out[lo_idx] === 1'b1 && c < 20000) begin
				@(negedge aclk); c = c + 1;
			end
			t_high_lo = c;

			c = 0;
			while (pwm_out[hi_idx] !== 1'b1 && c < 20000) begin
				@(negedge aclk); c = c + 1;
			end
			gap_lh = c;

			t_period = t_high_hi + gap_hl + t_high_lo + gap_lh;
		end
	endtask

	//------------------------------------------------------------------
	// Count how many clocks of exactly one period each switch of a leg is on.
	// Unlike measure_leg this also works when a side never turns on.
	//------------------------------------------------------------------
	task count_leg_high;
		input  integer bridge;
		input  integer leg;
		output integer hi_cnt;
		output integer lo_cnt;
		integer hi_idx, lo_idx, kk;
		begin
			// leg B's outputs are cross connected in the RTL: BL carries the
			// half bridge's "high" window and BH carries the "low" window
			if (leg == 0) begin hi_idx = 4*bridge+0; lo_idx = 4*bridge+1; end
			else          begin hi_idx = 4*bridge+3; lo_idx = 4*bridge+2; end

			align_period;
			hi_cnt = 0;
			lo_cnt = 0;
			for (kk = 0; kk < per_m; kk = kk + 1) begin
				if (pwm_out[hi_idx] === 1'b1) hi_cnt = hi_cnt + 1;
				if (pwm_out[lo_idx] === 1'b1) lo_cnt = lo_cnt + 1;
				@(negedge aclk);
			end
		end
	endtask

	//------------------------------------------------------------------
	// Compare two pins over one full period and count how many clocks they
	// differ.  Used to prove the diagonal switches are in phase.
	//------------------------------------------------------------------
	task compare_pins;
		input  integer bridge;
		input  integer idx_x;
		input  integer idx_y;
		output integer diff;
		integer kk;
		begin
			align_period;
			diff = 0;
			for (kk = 0; kk < per_m; kk = kk + 1) begin
				if (pwm_out[idx_x] !== pwm_out[idx_y]) diff = diff + 1;
				@(negedge aclk);
			end
		end
	endtask

	//------------------------------------------------------------------
	// Capture one complete period of all 20 outputs
	//------------------------------------------------------------------
	task capture_period;
		input integer sel;
		integer kk;
		begin
			align_period;
			for (kk = 0; kk < per_m; kk = kk + 1) begin
				cap[sel][kk] = pwm_out;
				@(negedge aclk);
			end
		end
	endtask

	//------------------------------------------------------------------
	// Sample whole periods and compare all 20 outputs against the model.
	// The outputs are registered, so at the clock where cnt == kk the pins
	// carry f(kk-1); that one cycle of latency is modelled explicitly.
	//------------------------------------------------------------------
	task sample_period;
		input integer periods;
		integer p, kk, kp, guard, b, ci;
		reg exp_ah, exp_al, exp_bh, exp_bl;
		begin
			for (p = 0; p < periods; p = p + 1) begin
				guard = 0;
				@(negedge aclk);
				while (dut.pwm_cnt !== 32'd0 && guard < 20000) begin
					@(negedge aclk); guard = guard + 1;
				end

				for (kk = 0; kk < per_m; kk = kk + 1) begin
					kp = (kk == 0) ? (per_m - 1) : (kk - 1);

					for (b = 0; b < 5; b = b + 1) begin
						// The phase shift moves the COUNTER, not the waveform:
						// ci is what this bridge's half bridges compare against.
						// (Same expression as cnt_i in the top level, valid for
						//  phase_m[b] in [0, per_m].)
						ci = (kp >= phase_m[b]) ? (kp - phase_m[b])
						                        : (per_m - (phase_m[b] - kp));
						// leg A : AH is the "high" window, AL the "low" window
						exp_ah = (ci >= dta_m[b]) && (ci < duty_m[b]);
						exp_al = (ci >= duty_m[b]) &&
						         ((ci - duty_m[b]) >= dta_m[b]);
						// leg B is cross connected: BL takes the "high" window.
						// In a static mode leg B ignores duty and dead time.
						if (mode_m[b] == 2'd1) begin
							exp_bl = 1'b1;		// static LOW : BL on, BH off
							exp_bh = 1'b0;
						end else if (mode_m[b] == 2'd2) begin
							exp_bl = 1'b0;		// static HIGH: BH on, BL off
							exp_bh = 1'b1;
						end else begin
							exp_bl = (ci >= dtb_m[b]) && (ci < duty_m[b]);
							exp_bh = (ci >= duty_m[b]) &&
							         ((ci - duty_m[b]) >= dtb_m[b]);
						end

						if (inv_m) begin
							exp_ah = ~exp_ah; exp_al = ~exp_al;
							exp_bh = ~exp_bh; exp_bl = ~exp_bl;
						end
						if (!en_m[2*b])   begin exp_ah = 1'b0; exp_al = 1'b0; end
						if (!en_m[2*b+1]) begin exp_bh = 1'b0; exp_bl = 1'b0; end

						if (pwm_out[4*b+0] !== exp_ah ||
						    pwm_out[4*b+1] !== exp_al ||
						    pwm_out[4*b+2] !== exp_bh ||
						    pwm_out[4*b+3] !== exp_bl) begin
							phase_err = phase_err + 1;
							if (phase_err <= 5)
								$display("  FAIL  model br%0d cnt %0d : got AH%0b AL%0b BH%0b BL%0b  exp AH%0b AL%0b BH%0b BL%0b (en=%b inv=%b)",
								         b, kk,
								         pwm_out[4*b+0], pwm_out[4*b+1],
								         pwm_out[4*b+2], pwm_out[4*b+3],
								         exp_ah, exp_al, exp_bh, exp_bl, en_m, inv_m);
						end
					end
					@(negedge aclk);
				end
			end
		end
	endtask

	// random helper
	function integer rnd;
		input integer lo;
		input integer hi;
		integer r;
		begin
			r = {$random} % (hi - lo + 1);
			rnd = lo + r;
		end
	endfunction

	//------------------------------------------------------------------
	// Write every register from the model arrays
	//------------------------------------------------------------------
	//------------------------------------------------------------------
	// Measure the value of the free running counter on the clock where a pin
	// makes a true low -> high transition.
	//
	// Two bridges whose phase offsets differ by PHASE give rise counts that
	// differ by exactly PHASE (modulo the period).  This is far more robust
	// than counting clocks between two edges: at 50 % duty a pin may already be
	// high when you start looking, so a naive edge delay can come out as 0.
	//------------------------------------------------------------------
	task measure_rise_cnt;
		input  integer idx;
		output integer cnt_at;
		integer c;
		begin
			c = 0;
			while (pwm_out[idx] === 1'b1 && c < 20000) begin
				@(negedge aclk); c = c + 1;
			end
			c = 0;
			while (pwm_out[idx] !== 1'b1 && c < 20000) begin
				@(negedge aclk); c = c + 1;
			end
			cnt_at = dut.pwm_cnt;
		end
	endtask

	//------------------------------------------------------------------
	// Count how many times a pin changes level over exactly one period.
	// A pin held static by a leg-B static mode must give 0.
	//------------------------------------------------------------------
	task count_pin_edges;
		input  integer idx;
		output integer edges;
		integer kk;
		reg prev;
		begin
			align_period;
			edges = 0;
			prev  = pwm_out[idx];
			for (kk = 0; kk < per_m; kk = kk + 1) begin
				@(negedge aclk);
				if (pwm_out[idx] !== prev) edges = edges + 1;
				prev = pwm_out[idx];
			end
		end
	endtask

	task program_model;
		begin
			// pack the five 2-bit leg B modes into PWM_EN[19:10]
			mode_vec = {mode_m[4][1:0], mode_m[3][1:0], mode_m[2][1:0],
			            mode_m[1][1:0], mode_m[0][1:0]};
			axi_write(A_PERIOD, per_m[31:0]);
			axi_write(A_DUTY0,  duty_m[0][31:0]);
			axi_write(A_DUTY1,  duty_m[1][31:0]);
			axi_write(A_DUTY2,  duty_m[2][31:0]);
			axi_write(A_DUTY3,  duty_m[3][31:0]);
			axi_write(A_DUTY4,  duty_m[4][31:0]);
			// DEADx packing is {leg B[31:16], leg A[15:0]}
			axi_write(A_DEAD0, {dtb_m[0][15:0], dta_m[0][15:0]});
			axi_write(A_DEAD1, {dtb_m[1][15:0], dta_m[1][15:0]});
			axi_write(A_DEAD2, {dtb_m[2][15:0], dta_m[2][15:0]});
			axi_write(A_DEAD3, {dtb_m[3][15:0], dta_m[3][15:0]});
			axi_write(A_DEAD4, {dtb_m[4][15:0], dta_m[4][15:0]});
			axi_write(A_PWM_EN, {12'd0, mode_vec, en_m});
			axi_write(A_PHASE0, phase_m[0][31:0]);
			axi_write(A_PHASE1, phase_m[1][31:0]);
			axi_write(A_PHASE2, phase_m[2][31:0]);
			axi_write(A_PHASE3, phase_m[3][31:0]);
			axi_write(A_PHASE4, phase_m[4][31:0]);
		end
	endtask

	//------------------------------------------------------------------
	// Stimulus
	//------------------------------------------------------------------
	integer t_hh, g_hl, t_hl, g_lh, t_per;
	integer h_cnt, l_cnt, bad, diff, distinct, prev16;
	integer exp_h, exp_l;
	integer rise0, risek, diffc;
	integer edges;
	reg [31:0] got32;
	// snapshot used by the overcurrent tests to prove the non-faulted bridges
	// are not disturbed by the combinational mask
	reg [19:0] prev_pwm;
	reg [9:0]  prev_en;

	initial begin
		$display("============================================================");
		$display(" pwm_axi_ip self-checking testbench (5 x H bridge, 20 PWM)");
		$display("============================================================");

		aresetn = 1'b0;
		repeat (10) @(negedge aclk);
		aresetn = 1'b1;
		repeat (5) @(negedge aclk);

		// every test runs at phase 0 / normal leg B mode unless it sets otherwise
		for (k = 0; k < 5; k = k + 1) begin
			phase_m[k] = 0;
			mode_m[k]  = 0;
		end

		//--------------------------------------------------------------
		$display("[T1] reset state : all 20 outputs must be inactive");
		check("T1 pwm_out low after reset ", {12'd0, pwm_out}, 32'h00000000);
		check("T1 gate_en low after reset ", {22'd0, gate_en}, 32'h00000000);

		//--------------------------------------------------------------
		$display("[T2] ID register");
		axi_read(A_ID, got32);
		check("T2 ID = PWM1", got32, IP_ID_EXPECTED);

		//--------------------------------------------------------------
		$display("[T3] register write / read-back");
		axi_write(A_PERIOD, 32'd1000);      axi_read(A_PERIOD, got32); check("T3 PERIOD", got32, 32'd1000);
		axi_write(A_DUTY0,  32'd400);       axi_read(A_DUTY0,  got32); check("T3 DUTY0",  got32, 32'd400);
		axi_write(A_DUTY1,  32'd250);       axi_read(A_DUTY1,  got32); check("T3 DUTY1",  got32, 32'd250);
		axi_write(A_DUTY2,  32'd100);       axi_read(A_DUTY2,  got32); check("T3 DUTY2",  got32, 32'd100);
		axi_write(A_DUTY3,  32'd900);       axi_read(A_DUTY3,  got32); check("T3 DUTY3",  got32, 32'd900);
		axi_write(A_DUTY4,  32'd50);        axi_read(A_DUTY4,  got32); check("T3 DUTY4",  got32, 32'd50);
		// DEADx packing is {leg B[31:16], leg A[15:0]}
		axi_write(A_DEAD0, 32'h0014_000A);  axi_read(A_DEAD0,  got32); check("T3 DEAD0 {B,A}", got32, 32'h0014000A);
		axi_write(A_DEAD1, 32'h0007_001F);  axi_read(A_DEAD1,  got32); check("T3 DEAD1 {B,A}", got32, 32'h0007001F);
		axi_write(A_DEAD2, 32'h0064_0000);  axi_read(A_DEAD2,  got32); check("T3 DEAD2 {B,A}", got32, 32'h00640000);
		axi_write(A_DEAD3, 32'h0000_0000);  axi_read(A_DEAD3,  got32); check("T3 DEAD3 {B,A}", got32, 32'h00000000);
		axi_write(A_DEAD4, 32'h00C8_00C8);  axi_read(A_DEAD4,  got32); check("T3 DEAD4 {B,A}", got32, 32'h00C800C8);
		axi_write(A_PWM_EN, 32'h0000_03FF); axi_read(A_PWM_EN, got32); check("T3 PWM_EN", got32, 32'h000003FF);
		axi_write(A_SCRATCH,32'hDEAD_BEEF); axi_read(A_SCRATCH,got32); check("T3 SCRATCH", got32, 32'hDEADBEEF);
		axi_write(A_CTRL,   32'h0000_0001); axi_read(A_CTRL,   got32); check("T3 CTRL", got32, 32'h00000001);
		axi_write(A_PWM_EN, 32'hFFFF_FC00);
		axi_read(A_PWM_EN, got32); check("T3 PWM_EN read-back", got32, 32'hFFFFFC00);
		@(negedge aclk);
		check("T3 PWM_EN only [9:0] are used", {22'd0, gate_en}, 32'h00000000);

		//--------------------------------------------------------------
		$display("[T4] write strobes");
		axi_write(A_SCRATCH, 32'hFFFFFFFF);
		axi_read(A_SCRATCH, got32); check("T4 all bytes set", got32, 32'hFFFFFFFF);
		axi_write_strobe(A_SCRATCH, 32'h00000000, 4'b0001);
		axi_read(A_SCRATCH, got32); check("T4 clear byte0 only", got32, 32'hFFFFFF00);
		axi_write_strobe(A_SCRATCH, 32'h00000000, 4'b0100);
		axi_read(A_SCRATCH, got32); check("T4 clear byte2 only", got32, 32'hFF00FF00);
		axi_write_strobe(A_SCRATCH, 32'h00000000, 4'b0010);
		axi_read(A_SCRATCH, got32); check("T4 clear byte1 only", got32, 32'hFF000000);
		axi_write_strobe(A_SCRATCH, 32'h00000000, 4'b1000);
		axi_read(A_SCRATCH, got32); check("T4 clear byte3 only", got32, 32'h00000000);

		//--------------------------------------------------------------
		$display("[T5] read-only registers");
		axi_write(A_ID, 32'h00000000);
		axi_read(A_ID, got32); check("T5 ID unchanged after write", got32, IP_ID_EXPECTED);
		axi_write(A_STATUS, 32'hFFFFFFFF);
		axi_read(A_STATUS, got32);
		check("T5 STATUS reserved bits stay 0", {20'd0, got32[15:4]}, 32'd0);
		distinct = 0;
		prev16   = got32[31:16];
		for (k = 0; k < 40; k = k + 1) begin
			axi_read(A_STATUS, got32);
			if (got32[31:16] !== prev16) distinct = 1;
			prev16 = got32[31:16];
		end
		check_int("T5 STATUS exposes a live counter", distinct, 1);

		//--------------------------------------------------------------
		// Canonical configuration = the REAL operating point
		//   clock 50 MHz, PWM 4 kHz  -> PERIOD = 50e6/4000 = 12500 counts
		//                                = 250 us per period
		//   dead time 100 ns         -> DEAD   = 100/20    = 5 counts
		//   one duty per BRIDGE (both legs share it, so AH stays in phase
		//   with BL), ladder 10 / 30 / 50 / 70 / 90 %
		//     -> DUTY = 1250 / 3750 / 6250 / 8750 / 11250
		// Edge cases (duty below the dead time, duty above the period, very
		// small periods) are covered by the T17 random sweep instead, so that
		// this canonical configuration stays the one you actually ship.
		//--------------------------------------------------------------
		per_m  = 12500;
		duty_m[0]=1250;  dta_m[0]=5; dtb_m[0]=5;
		duty_m[1]=3750;  dta_m[1]=5; dtb_m[1]=5;
		duty_m[2]=6250;  dta_m[2]=5; dtb_m[2]=5;
		duty_m[3]=8750;  dta_m[3]=5; dtb_m[3]=5;
		duty_m[4]=11250; dta_m[4]=5; dtb_m[4]=5;
		en_m  = 10'h3FF;
		inv_m = 1'b0;

		axi_write(A_CTRL, 32'h0000_0000);		// stop while configuring
		program_model;
		axi_write(A_CTRL, 32'h0000_0001);		// GLB_EN = 1
		repeat (4) @(negedge aclk);

		$display("[T6] bridge 0 leg A @ 4 kHz, DUTY=10 %%, DEAD=100 ns");
		measure_leg(0, 0, t_hh, g_hl, t_hl, g_lh, t_per);
		check_int("T6 period = 12500 (4 kHz)", t_per, 12500);
		check_int("T6 AH high = DUTY-DEAD   ", t_hh, 1250-5);
		check_int("T6 gap AH->AL = DEAD     ", g_hl, 5);
		check_int("T6 AL high               ", t_hl, 12500-1250-5);
		check_int("T6 gap AL->AH = DEAD     ", g_lh, 5);

		$display("[T7] bridge 0 leg B (its own DEAD register, same value here)");
		measure_leg(0, 1, t_hh, g_hl, t_hl, g_lh, t_per);
		check_int("T7 period = 12500 (4 kHz)", t_per, 12500);
		check_int("T7 BL high = DUTY-DEAD   ", t_hh, 1250-5);
		check_int("T7 gap BL->BH = DEAD     ", g_hl, 5);
		check_int("T7 BH high               ", t_hl, 12500-1250-5);
		check_int("T7 gap BH->BL = DEAD     ", g_lh, 5);

		$display("[T8] all 5 bridges carry their own duty and dead times");
		for (k = 0; k < 5; k = k + 1) begin
			count_leg_high(k, 0, h_cnt, l_cnt);
			exp_h = duty_m[k];
			if (exp_h > per_m) exp_h = per_m;
			exp_h = exp_h - dta_m[k];
			if (exp_h < 0) exp_h = 0;
			exp_l = per_m - duty_m[k] - dta_m[k];
			if (exp_l < 0) exp_l = 0;
			check_int("T8 leg A high side on-time", h_cnt, exp_h);
			check_int("T8 leg A low  side on-time", l_cnt, exp_l);

			count_leg_high(k, 1, h_cnt, l_cnt);
			exp_h = duty_m[k];
			if (exp_h > per_m) exp_h = per_m;
			exp_h = exp_h - dtb_m[k];
			if (exp_h < 0) exp_h = 0;
			exp_l = per_m - duty_m[k] - dtb_m[k];
			if (exp_l < 0) exp_l = 0;
			check_int("T8 leg B high side on-time", h_cnt, exp_h);
			check_int("T8 leg B low  side on-time", l_cnt, exp_l);
		end

		//--------------------------------------------------------------
		$display("[T9/T10] the diagonal switches must be IN PHASE");
		for (k = 0; k < 5; k = k + 1) begin
			dta_m[k] = 5;			// both legs at the operating point dead
			dtb_m[k] = 5;			// time -> AH/BL exactly edge aligned
		end
		program_model;
		repeat (4) @(negedge aclk);

		for (k = 0; k < 5; k = k + 1) begin
			compare_pins(k, AH(k), BL(k), diff);
			check_int("T9  AH == BL over one period", diff, 0);
			compare_pins(k, AL(k), BH(k), diff);
			check_int("T10 AL == BH over one period", diff, 0);
			compare_pins(k, AH(k), AL(k), diff);
			check_int("T9  AH != AL (complementary)", (diff > 0), 1);
		end

		//--------------------------------------------------------------
		$display("[T11] gate_en matches the PWM_EN bit mapping");
		axi_write(A_PWM_EN, 32'h0000_0155);		// 0b01_0101_0101
		en_m = 10'h155;
		@(negedge aclk);
		check("T11 gate_en == PWM_EN", {22'd0, gate_en}, 32'h00000155);

		//--------------------------------------------------------------
		$display("[T12] clearing one leg enable only affects that leg");
		en_m = 10'h3FE;					// all enables except bit0 (bridge 0 leg A)
		program_model;
		repeat (4) @(negedge aclk);
		bad = 0;
		for (k = 0; k < 1200; k = k + 1) begin
			@(negedge aclk);
			if (pwm_out[0] !== 1'b0 || pwm_out[1] !== 1'b0) bad = bad + 1;
		end
		check_int("T12 disabled leg A pins stay low", bad, 0);
		check("T12 gate_en bit0 cleared", {22'd0, gate_en}, 32'h000003FE);
		count_leg_high(0, 1, h_cnt, l_cnt);
		check_int("T12 leg B high side keeps running", h_cnt, 1250-5);
		count_leg_high(1, 0, h_cnt, l_cnt);
		check_int("T12 other bridge unaffected", h_cnt, 3750-5);

		//--------------------------------------------------------------
		$display("[T13] global enable");
		en_m = 10'h3FF;
		program_model;
		axi_write(A_CTRL, 32'h0000_0000);		// GLB_EN = 0
		repeat (4) @(negedge aclk);
		bad = 0;
		for (k = 0; k < 1200; k = k + 1) begin
			@(negedge aclk);
			if (pwm_out !== 20'h0) bad = bad + 1;
		end
		check_int("T13 all 20 pwm_out low when GLB_EN=0", bad, 0);
		check("T13 gate_en unaffected by GLB_EN", {22'd0, gate_en}, 32'h000003FF);

		//--------------------------------------------------------------
		$display("[T14] PERIOD=0 parks the outputs");
		axi_write(A_CTRL, 32'h0000_0001);
		axi_write(A_PERIOD, 32'd0);
		repeat (4) @(negedge aclk);
		bad = 0;
		for (k = 0; k < 200; k = k + 1) begin
			@(negedge aclk);
			if (pwm_out !== 20'h0) bad = bad + 1;
		end
		check_int("T14 all 20 pwm_out low when PERIOD=0", bad, 0);
		axi_write(A_PERIOD, per_m[31:0]);

		//--------------------------------------------------------------
		$display("[T15] INV_PWM produces the exact complement");
		inv_m = 1'b0;
		axi_write(A_CTRL, 32'h0000_0001);
		capture_period(0);
		inv_m = 1'b1;
		axi_write(A_CTRL, 32'h0000_0003);
		capture_period(1);
		bad = 0;
		for (k = 0; k < per_m; k = k + 1) begin
			if (cap[1][k] !== ~cap[0][k]) begin
				bad = bad + 1;
				if (bad <= 3)
					$display("  FAIL  INV sample %0d : got %b expected %b", k, cap[1][k], ~cap[0][k]);
			end
		end
		check_int("T15 INV_PWM samples differing", bad, 0);
		inv_m = 1'b0;

		//--------------------------------------------------------------
		$display("[T16] whole period compared against the model");
		program_model;
		axi_write(A_CTRL, 32'h0000_0001);
		sample_period(3);
		check_int("T16 model mismatches over 3 periods", phase_err, 0);

		//--------------------------------------------------------------
		$display("[T17] randomised register sweep (safety monitor running)");
		for (k = 0; k < 60; k = k + 1) begin
			per_m = rnd(60, 400);
			for (i = 0; i < 5; i = i + 1) begin
				duty_m[i] = rnd(0, per_m + 50);		// may exceed the period
				dta_m[i]  = rnd(0, 300);		// may exceed the period
				dtb_m[i]  = rnd(0, 300);
				phase_m[i] = rnd(0, per_m - 1);		// kept inside [0, PERIOD)
				mode_m[i]  = rnd(0, 2);			// normal / static low / static high
			end
			en_m  = rnd(0, 1023);
			inv_m = k[0];
			program_model;
			axi_write(A_CTRL, (k[0] ? 32'h00000003 : 32'h00000001));
			sample_period(1);
		end
		check_int("T17 model mismatches during sweep", phase_err, 0);
		check_int("T17 shoot-through events (whole run)", shoot_err, 0);

		//--------------------------------------------------------------
		// T18 : the REAL operating point, not a random one.
		//   50 MHz clock, 4 kHz PWM  -> PERIOD = 12500 counts = 250 us
		//   dead time 100 ns         -> DEAD   = 5 counts
		//   duty swept 10 % .. 100 % in 5 steps, one whole 250 us period each
		// A 250 us period is 20x longer than anything T17 uses, so this also
		// proves the 32 bit counter and comparators behave at the real value.
		//--------------------------------------------------------------
		$display("[T18] real operating point: 4 kHz, DEAD=100 ns, DUTY 10..100 %%");
		per_m = 12500;
		for (k = 0; k < 5; k = k + 1) begin
			dta_m[k] = 5;
			dtb_m[k] = 5;
			mode_m[k] = 0;				// reset: T17 leaves this random
		end
		en_m  = 10'h3FF;
		inv_m = 1'b0;
		for (p = 0; p < 5; p = p + 1) begin
			// 10 %, 30 %, 50 %, 80 %, 100 %
			case (p)
			  0: k = 10;
			  1: k = 30;
			  2: k = 50;
			  3: k = 80;
			  default: k = 100;
			endcase
			for (i = 0; i < 5; i = i + 1) begin
				duty_m[i] = (per_m * k) / 100;
				if (duty_m[i] > per_m) duty_m[i] = per_m;	// never above 100 %
			end
			$display("   duty %3d %%  -> DUTY = %0d of %0d", k, duty_m[0], per_m);
			program_model;
			axi_write(A_CTRL, 32'h00000001);
			sample_period(1);
		end
		check_int("T18 model mismatches at the operating point", phase_err, 0);
		check_int("T18 shoot-through at the operating point", shoot_err, 0);

		//--------------------------------------------------------------
		// T19 : overcurrent protection
		//   Five active-low inputs, combinational shutdown, sticky status,
		//   acknowledged by a CTRL.GLB_EN 0 -> 1 edge.  No new registers.
		//--------------------------------------------------------------
		$display("[T19] overcurrent protection (5 inputs, combinational shutdown)");
		for (k = 0; k < 5; k = k + 1) begin
			duty_m[k] = 6250;			// 50 %
			dta_m[k] = 5;
			dtb_m[k] = 5;
			mode_m[k] = 0;				// reset: T17 leaves this random
		end
		en_m  = 10'h3FF;
		inv_m = 1'b0;
		program_model;
		oc_n = 5'h1F;					// all healthy
		axi_write(A_CTRL, 32'h00000000);
		axi_write(A_CTRL, 32'h00000001);		// GLB_EN 0->1 also clears old faults
		repeat (6) @(negedge aclk);
		check_int("T19  PWM running before the fault", (pwm_out !== 20'h0), 1);

		// 19a : the shutdown path is COMBINATIONAL and it is PER BRIDGE.
		//       Assert the fault between two clock edges: bridge 0's four pins
		//       must be off 1 ns later (no clock, no register write), while the
		//       other four bridges still carry exactly the value they had.
		@(negedge aclk);
		prev_pwm = pwm_out;
		prev_en  = gate_en;
		oc_n = 5'b11110;				// bridge 0 overcurrent
		#1;
		check("T19a bridge0 pins off 1 ns later",   {16'd0, pwm_out[3:0]},  32'h00000000);
		check("T19a bridge0 gate_en off 1 ns later",{6'd0,  gate_en[1:0]},  32'h00000000);
		check("T19a bridge1-4 pins untouched",      {16'd0, pwm_out[19:4]}, {16'd0, prev_pwm[19:4]});
		check("T19a bridge1-4 gate_en untouched",   {6'd0,  gate_en[9:2]},  {6'd0,  prev_en[9:2]});

		// 19b : sticky status
		repeat (6) @(negedge aclk);
		axi_read(A_STATUS, got32);
		check("T19b STATUS.OC_NOW",       {31'd0, got32[4]},    32'd1);
		check("T19b STATUS.OC_LATCHED",   {31'd0, got32[5]},    32'd1);
		check("T19b STATUS.OC_SRC[0]",    {31'd0, got32[8]},    32'd1);
		check("T19b STATUS.OC_SRC others",{27'd0, got32[12:9]}, 32'd0);

		// 19c : releasing the input must NOT restart the tripped bridge, while
		//       the other four were never affected in the first place
		oc_n = 5'h1F;
		repeat (30) @(negedge aclk);
		check("T19c bridge0 still off after release", {16'd0, pwm_out[3:0]}, 32'h00000000);
		count_leg_high(1, 0, h_cnt, l_cnt);
		check_int("T19c bridge1 kept running all along", h_cnt, 6250-5);

		// 19d : software acknowledge = a deliberate GLB_EN 0 -> 1 edge
		axi_write(A_CTRL, 32'h00000000);		// GLB_EN = 0
		axi_write(A_CTRL, 32'h00000001);		// GLB_EN = 1  -> acknowledge
		repeat (4) @(negedge aclk);
		axi_read(A_STATUS, got32);
		check("T19d OC_LATCHED cleared", {31'd0, got32[5]},    32'd0);
		check("T19d OC_SRC cleared",     {27'd0, got32[12:8]}, 32'd0);
		count_leg_high(0, 0, h_cnt, l_cnt);
		check_int("T19d bridge0 resumed after the acknowledge", h_cnt, 6250-5);

		// 19e : with INV_PWM the OFF level is HIGH.  Forcing the pins LOW on an
		//       active-low driver would turn every switch ON - this is the trap
		//       the mask has to avoid.
		axi_write(A_CTRL, 32'h00000003);		// GLB_EN=1 + INV_PWM=1
		oc_n = 5'h1F;
		repeat (6) @(negedge aclk);
		@(negedge aclk);
		prev_pwm = pwm_out;
		oc_n = 5'b11110;
		#1;
		check("T19e INV_PWM: bridge0 off level is HIGH", {16'd0, pwm_out[3:0]},  32'h0000000F);
		check("T19e INV_PWM: bridge0 gate_en low",       {6'd0,  gate_en[1:0]},  32'h00000000);
		check("T19e INV_PWM: bridge1-4 untouched",       {16'd0, pwm_out[19:4]}, {16'd0, prev_pwm[19:4]});
		oc_n = 5'h1F;
		axi_write(A_CTRL, 32'h00000000);
		axi_write(A_CTRL, 32'h00000001);		// acknowledge, normal polarity back
		repeat (6) @(negedge aclk);

		// 19f : every input maps to its own OC_SRC bit
		bad = 0;
		for (k = 0; k < 5; k = k + 1) begin
			oc_n = 5'h1F;
			axi_write(A_CTRL, 32'h00000000);
			axi_write(A_CTRL, 32'h00000001);	// clear the latch first
			repeat (6) @(negedge aclk);
			oc_n = ~(5'h01 << k);			// trip input k only
			repeat (8) @(negedge aclk);
			axi_read(A_STATUS, got32);
			if (got32[8+k] !== 1'b1) bad = bad + 1;
			if (got32[12:8] !== (5'h01 << k)) bad = bad + 1;
		end
		oc_n = 5'h1F;
		check_int("T19f each input sets its own OC_SRC bit", bad, 0);

		//--------------------------------------------------------------
		// 19g : the whole point of the per-bridge mask - trip ONE bridge and
		//       prove the other four keep switching with their own waveform.
		//--------------------------------------------------------------
		$display("[T19g] only the faulted bridge stops");
		oc_n = 5'h1F;
		axi_write(A_CTRL, 32'h00000000);
		axi_write(A_CTRL, 32'h00000001);		// acknowledge
		repeat (6) @(negedge aclk);
		oc_n = ~(5'h01 << 2);				// bridge 2 overcurrent
		repeat (8) @(negedge aclk);
		bad = 0;
		for (k = 0; k < 1200; k = k + 1) begin
			@(negedge aclk);
			if (pwm_out[11:8] !== 4'h0) bad = bad + 1;	// bridge 2 pins
		end
		check_int("T19g faulted bridge held off", bad, 0);
		count_leg_high(0, 0, h_cnt, l_cnt);
		check_int("T19g bridge0 still switching", h_cnt, 6250-5);
		count_leg_high(1, 0, h_cnt, l_cnt);
		check_int("T19g bridge1 still switching", h_cnt, 6250-5);
		count_leg_high(3, 0, h_cnt, l_cnt);
		check_int("T19g bridge3 still switching", h_cnt, 6250-5);
		count_leg_high(4, 0, h_cnt, l_cnt);
		check_int("T19g bridge4 still switching", h_cnt, 6250-5);

		// IMPORTANT: release the fault input before the acknowledge below.
		// Leaving oc_n asserting bridge 2 keeps that bridge shut for the rest of
		// the run, which shows up later as "bridge 2 outputs are always 0".
		oc_n = 5'h1F;

		//--------------------------------------------------------------
		// T20 : phase offset must NOT break per-leg complementarity.
		//
		//   The phase shift moves the COUNTER each bridge compares against, not
		//   the waveform, and both legs of a bridge use the same shifted
		//   counter.  So inside a leg the two switches are still separated by
		//   exactly DEAD with the same DUTY, and AH is still in phase with BL.
		//   This test measures all of that rather than assuming it.
		//--------------------------------------------------------------
		$display("[T20] phase offset - complementarity must survive");
		per_m  = 12500;
		for (k = 0; k < 5; k = k + 1) begin
			duty_m[k]  = 6250;			// 50 %
			dta_m[k]   = 5;				// 100 ns
			dtb_m[k]   = 5;
			phase_m[k] = (per_m * k) / 5;		// 0/2500/5000/7500/10000
			mode_m[k]  = 0;				// reset: T17 leaves this random
		end
		en_m  = 10'h3FF;
		inv_m = 1'b0;
		axi_write(A_CTRL, 32'h00000000);
		program_model;
		axi_write(A_CTRL, 32'h00000001);
		repeat (6) @(negedge aclk);

		// diagnostic: what did the phase registers actually receive?
		axi_read(A_PHASE0, got32); check("T20 PHASE0 read back", got32, phase_m[0][31:0]);
		axi_read(A_PHASE1, got32); check("T20 PHASE1 read back", got32, phase_m[1][31:0]);
		axi_read(A_PHASE2, got32); check("T20 PHASE2 read back", got32, phase_m[2][31:0]);
		axi_read(A_PHASE3, got32); check("T20 PHASE3 read back", got32, phase_m[3][31:0]);
		axi_read(A_PHASE4, got32); check("T20 PHASE4 read back", got32, phase_m[4][31:0]);
		$display("  dut.phase = %0d %0d %0d %0d %0d", dut.phase[0], dut.phase[1],
		         dut.phase[2], dut.phase[3], dut.phase[4]);
		$display("  cnt_i     = %0d %0d %0d %0d %0d   (pwm_cnt = %0d)",
		         dut.g_bridge[0].cnt_i, dut.g_bridge[1].cnt_i, dut.g_bridge[2].cnt_i,
		         dut.g_bridge[3].cnt_i, dut.g_bridge[4].cnt_i, dut.pwm_cnt);

		// (a) period, on-time and BOTH dead gaps of leg A are identical on
		//     every bridge, no matter what its phase is
		for (k = 0; k < 5; k = k + 1) begin
			measure_leg(k, 0, t_hh, g_hl, t_hl, g_lh, t_per);
			check_int("T20 period unchanged by phase ", t_per, 12500);
			check_int("T20 leg A high side on-time  ", t_hh, 6250-5);
			check_int("T20 leg A gap up   = 100 ns  ", g_hl, 5);
			check_int("T20 leg A low side on-time   ", t_hl, 12500-6250-5);
			check_int("T20 leg A gap down = 100 ns  ", g_lh, 5);
		end

		// (b) the diagonals are still in phase and a leg is still complementary
		for (k = 0; k < 5; k = k + 1) begin
			compare_pins(k, AH(k), BL(k), diff);
			check_int("T20 AH == BL under phase offset", diff, 0);
			compare_pins(k, AL(k), BH(k), diff);
			check_int("T20 AL == BH under phase offset", diff, 0);
			compare_pins(k, AH(k), AL(k), diff);
			check_int("T20 AH != AL still complementary", (diff > 0), 1);
		end

		// (c) the interleaving actually happens: the counter value at which each
		//     bridge's high side rises must differ by exactly the phase offset
		measure_rise_cnt(AH(0), rise0);
		for (k = 1; k < 5; k = k + 1) begin
			measure_rise_cnt(AH(k), risek);
			diffc = risek - rise0;
			if (diffc < 0) diffc = diffc + per_m;
			check_int("T20 interleave == PHASEk", diffc, phase_m[k]);
		end

		// (d) the whole period still matches the model, now with phase included
		sample_period(1);
		check_int("T20 model mismatches with phase", phase_err, 0);
		check_int("T20 shoot-through with phase   ", shoot_err, 0);

		// (e) back to phase 0 for the closing state
		for (k = 0; k < 5; k = k + 1) phase_m[k] = 0;
		program_model;
		repeat (4) @(negedge aclk);

		//--------------------------------------------------------------
		// T21 : eddy current brake / coil drive states.
		//
		//   A coil wants a smooth UNIDIRECTIONAL current, not the bipolar
		//   (diagonal) waveform a motor wants.  Every state a coil driver
		//   needs is reached by holding leg B STATIC and PWM-ing leg A - the
		//   classic "PWM + direction" H-bridge drive.  mode_m lives in
		//   PWM_EN[19:10], so this cost zero new registers.
		//--------------------------------------------------------------
		$display("[T21] eddy current brake / coil drive states");
		per_m = 12500;
		for (k = 0; k < 5; k = k + 1) begin
			phase_m[k] = 0;
			dta_m[k]   = 5;
			dtb_m[k]   = 5;			// deliberately non-zero: a static mode
		end					// must IGNORE it, see (e)
		en_m  = 10'h3FF;
		inv_m = 1'b0;

		// (a) PURE DC : DUTY = PERIOD, no leg A dead time, leg B static LOW.
		//     AH and BL on for the whole period -> ZERO switching, ZERO ripple.
		//     The ideal state for a brake that just needs a fixed field.
		for (k = 0; k < 5; k = k + 1) begin
			duty_m[k] = per_m;
			dta_m[k]  = 0;
			mode_m[k] = 1;
		end
		axi_write(A_CTRL, 32'h00000000);
		program_model;
		axi_write(A_CTRL, 32'h00000001);
		repeat (6) @(negedge aclk);
		sample_period(1);
		check_int("T21a model mismatches in pure DC ", phase_err, 0);
		count_pin_edges(AH(0), edges);
		check_int("T21a AH has no edges in pure DC", edges, 0);
		count_pin_edges(BL(0), edges);
		check_int("T21a BL static (no edges)       ", edges, 0);
		count_pin_edges(BH(0), edges);
		check_int("T21a BH static (no edges)       ", edges, 0);

		// (b) CHOPPER with SLOW DECAY : DUTY = 25 %, leg B static LOW.
		//     AH chops, AL freewheels through the two LOW side switches, so the
		//     load sees +Vbus / 0 V instead of the +Vbus / -Vbus that the
		//     diagonal mode gives.  That is the low-ripple hold current.
		for (k = 0; k < 5; k = k + 1) begin
			duty_m[k] = 3125;		// 25 %
			dta_m[k]  = 5;
			mode_m[k] = 1;
		end
		program_model;
		repeat (6) @(negedge aclk);
		sample_period(1);
		check_int("T21b model mismatches in chopper", phase_err, 0);
		count_pin_edges(BL(0), edges);
		check_int("T21b BL stays static while AH chops", edges, 0);
		measure_leg(0, 0, t_hh, g_hl, t_hl, g_lh, t_per);
		check_int("T21b period                ", t_per, 12500);
		check_int("T21b AH on-time = DUTY-DEAD", t_hh, 3125-5);

		// (c) FAST RELEASE : leg A static low, leg B static HIGH -> -Vbus.
		//     This forces the coil current down far faster than the L/R decay.
		for (k = 0; k < 5; k = k + 1) begin
			duty_m[k] = 0;
			dta_m[k]  = 0;
			mode_m[k] = 2;
		end
		program_model;
		repeat (6) @(negedge aclk);
		sample_period(1);
		check_int("T21c model mismatches in fast release", phase_err, 0);
		// bridge 0 pins {BL,BH,AL,AH} = {0,1,1,0} : A low, B high
		check("T21c -Vbus: AL on, BH on", {12'd0, pwm_out[3:0]}, 32'h00000006);

		// (d) ACTIVE SHORT : both legs static LOW -> both terminals at GND
		for (k = 0; k < 5; k = k + 1) begin
			duty_m[k] = 0;
			dta_m[k]  = 0;
			mode_m[k] = 1;
		end
		program_model;
		repeat (6) @(negedge aclk);
		sample_period(1);
		check_int("T21d model mismatches in active short", phase_err, 0);
		// bridge 0 pins {BL,BH,AL,AH} = {1,0,1,0} : both low sides on
		check("T21d active short: AL on, BL on", {12'd0, pwm_out[3:0]}, 32'h0000000A);

		// (e) THE POINT OF THE STATIC MODES: leg B must be genuinely static.
		//     dtb_m is still 5 here on purpose - if the leg B dead time were not
		//     forced to zero, the "static" leg would drop out for 5 cycles every
		//     period and the coil would see a reverse spike each period instead
		//     of clean DC.  count_pin_edges already proved AH/BL/BH are flat in
		//     (a); prove it again for the reverse state.
		count_pin_edges(BH(0), edges);
		check_int("T21e BH static in the fast-release state", edges, 0);
		count_pin_edges(AL(0), edges);
		check_int("T21e AL static in the fast-release state", edges, 0);

		// (f) single mode change returns the bridge to normal diagonal drive
		//     (PWM_EN[19:10] = 0), i.e. the motor behaviour is untouched.
		for (k = 0; k < 5; k = k + 1) begin
			duty_m[k] = 6250;		// 50 %
			dta_m[k]  = 5;
			mode_m[k] = 0;
		end
		program_model;
		repeat (6) @(negedge aclk);
		sample_period(1);
		check_int("T21f model mismatches back in diagonal", phase_err, 0);
		compare_pins(0, AH(0), BL(0), diff);
		check_int("T21f diagonal restored: AH == BL", diff, 0);
		check_int("T21f shoot-through (whole run)", shoot_err, 0);

		// leave the IP healthy
		axi_write(A_CTRL, 32'h00000000);
		axi_write(A_CTRL, 32'h00000001);
		repeat (6) @(negedge aclk);

		//--------------------------------------------------------------
		$display("============================================================");
		if (fail_cnt == 0 && phase_err == 0 && shoot_err == 0)
			$display(" RESULT: ALL %0d CHECKS PASSED", pass_cnt);
		else
			$display(" RESULT: %0d FAILED, %0d PASSED (model_err=%0d shoot_err=%0d)",
			         fail_cnt, pass_cnt, phase_err, shoot_err);
		$display("============================================================");
		$finish;
	end

	// watchdog
	initial begin
		#40_000_000;
		$display(" FATAL: simulation watchdog timeout");
		$finish;
	end

endmodule
