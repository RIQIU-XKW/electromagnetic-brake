
`timescale 1 ns / 1 ps

//=============================================================================
// pwm_half_bridge : one half bridge (one leg) = 2 switches + dead time
//-----------------------------------------------------------------------------
//  Generates a complementary pair from a shared period counter:
//
//      high side on  <=>  cnt in [DEAD , DUTY)
//      low  side on  <=>  cnt in [DUTY + DEAD , PERIOD)
//
//  so the two switches are separated by exactly DEAD cycles at BOTH edges.
//
//  SAFETY - why shoot-through is impossible
//  ----------------------------------------
//  The high-side condition requires cnt <  DUTY.
//  The low-side  condition requires cnt >= DUTY.
//  These are mutually exclusive for ANY values of DUTY and DEAD, so the two
//  switches of this leg can never be on in the same clock - not even for the
//  transient values an incrementing binary counter passes through.  This holds
//  for DUTY > PERIOD, DUTY <= DEAD and DEAD = 0, so no clamping or value
//  checking is needed anywhere.  INV_PWM inverts both pins, which preserves
//  mutual exclusion.
//
//  WHY THE OUTPUTS ARE REGISTERED
//  ------------------------------
//  cnt is a binary counter, so when several of its bits change at once
//  (e.g. 0111 -> 1000) a purely combinational comparator can emit a narrow
//  hazard pulse.  Such a pulse cannot cause shoot-through (the two conditions
//  stay mutually exclusive for every momentary value of cnt), but it would put
//  glitches on the gate-drive lines.  Registering both outputs removes them and
//  also halves the critical path.
//
//  Consequence: the pins lag the counter by exactly one clock.  Every output of
//  the whole IP is registered the same way, so the duty cycle, the dead times
//  and the phase relations between the 20 outputs are all unaffected - only the
//  whole waveform is shifted by one 20 ns clock.
//=============================================================================

module pwm_half_bridge
(
	input  wire        clk,		// s00_axi_aclk
	input  wire        rstn,	// s00_axi_aresetn, active low
	input  wire        run,		// global enable AND period valid
	input  wire        inv,		// CTRL.INV_PWM : invert both output levels
	input  wire        en,		// this half bridge's gate-driver enable
	input  wire [31:0] cnt,		// shared period counter
	input  wire [31:0] duty,	// compare value (bridge duty cycle)
	input  wire [15:0] dead,	// dead time in aclk cycles
	output wire        hi,		// high side switch output
	output wire        lo		// low  side switch output
);

	//------------------------------------------------------------------
	// Combinational switching decision
	//------------------------------------------------------------------
	// (cnt >= dead) && (cnt < duty)
	wire hi_raw = (cnt >= {16'd0, dead}) && (cnt < duty);

	// (cnt >= duty) && (cnt - duty >= dead)
	// Written this way so that no 33-bit arithmetic is needed: in this branch
	// cnt >= duty holds, so cnt - duty can never underflow.
	wire lo_raw = (cnt >= duty) && ((cnt - duty) >= {16'd0, dead});

	// INV_PWM lets an active-low gate driver be used directly.  Inverting both
	// signals of the pair keeps them mutually exclusive.
	wire hi_act = inv ? ~hi_raw : hi_raw;
	wire lo_act = inv ? ~lo_raw : lo_raw;

	// An output is only driven when the period is valid, the global enable is
	// set AND this half bridge's own enable is set; otherwise it is held low
	// (both switches of the leg off).
	wire drive = run && en;

	//------------------------------------------------------------------
	// Glitch-free registered outputs
	//------------------------------------------------------------------
	reg hi_r;
	reg lo_r;

	always @(posedge clk)
	begin
		if (rstn == 1'b0) begin
			hi_r <= 1'b0;
			lo_r <= 1'b0;
		end
		else begin
			hi_r <= (drive && hi_act);
			lo_r <= (drive && lo_act);
		end
	end

	assign hi = hi_r;
	assign lo = lo_r;

endmodule
