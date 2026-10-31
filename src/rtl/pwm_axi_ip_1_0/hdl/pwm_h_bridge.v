
`timescale 1 ns / 1 ps

//=============================================================================
// pwm_h_bridge : one complete H bridge = 4 switches, built from 2 half bridges
//-----------------------------------------------------------------------------
//  Topology
//
//        +Vbus
//          |            |
//        [AH]         [BH]        AH / AL = leg A
//          |            |         BH / BL = leg B
//          +---- MOTOR --+
//          |            |
//        [AL]         [BL]
//          |            |
//         GND          GND
//
//  Outputs
//     ah : leg A high side        al : leg A low side   (complementary pair 1)
//     bh : leg B high side        bl : leg B low side   (complementary pair 2)
//
//  Phase relationship - this is the important part
//  -----------------------------------------------
//  A full bridge is driven in "lock anti-phase" (bipolar) mode: the two
//  DIAGONAL switches conduct together.
//
//     AH + BL on  -> current flows A -> B   ("forward diagonal")
//     AL + BH on  -> current flows B -> A   ("reverse diagonal")
//
//  so AH must be IN PHASE with BL, and AL in phase with BH.
//
//  That is achieved by CROSS-CONNECTING leg B: the half-bridge generator
//  produces an "upper window" and a "lower window", and for leg B the upper
//  window drives BL while the lower window drives BH.  Leg A is wired
//  straight through.  Both legs are therefore the SAME module - only the
//  output assignment differs, which is exactly what makes the bridge work.
//
//  Resulting waveform for bridge i (DUTY = D, leg dead times DTA / DTB):
//
//     cnt     0 ...... DTA-1 | DTA ...... D-1 | D ...... D+DTA-1 | D+DTA ...... PERIOD-1
//     AH               0      |       1        |        0        |        0
//     AL               0      |       0        |        0        |        1
//     BL               0      |       1        |        0        |        0
//     BH               0      |       0        |        0        |        1
//                            <-- DTA -->                 <-- DTA -->
//
//     (leg B is identical with DTB; set DTA = DTB and AH/BL are exactly
//      edge-aligned, which is the normal setting)
//
//  SAFETY
//  ------
//  Shoot-through can only happen INSIDE one leg, and pwm_half_bridge guarantees
//  the two switches of a leg are never on together.  AH and BL being on at the
//  same time is not shoot-through - it is the intended forward diagonal.
//  During every dead time all four switches are off and the motor current
//  freewheels through the body diodes.
//
//  NOTE
//  ----
//  Each half bridge registers its two outputs, so all four pins of this bridge
//  lag the counter by exactly one clock.  Since every bridge is registered the
//  same way, the duty cycle, the dead times and the phase relations are
//  unaffected - the whole 20-output waveform is simply shifted by one 20 ns
//  clock.  The waveform table above describes the combinational window.
//=============================================================================

module pwm_h_bridge
(
	input  wire        clk,
	input  wire        rstn,
	input  wire        run,		// global enable AND period valid
	input  wire        inv,		// invert all output levels
	input  wire [1:0]  en,		// {leg B enable, leg A enable}
	input  wire [31:0] cnt,		// shared period counter
	input  wire [31:0] duty,	// duty cycle (drives both legs in normal mode)
	input  wire [15:0] dead_a,	// dead time of leg A
	input  wire [15:0] dead_b,	// dead time of leg B (ignored in a static mode)
	input  wire [1:0]  mode_b,	// leg B mode, see below
	output wire        ah,		// leg A high side
	output wire        al,		// leg A low side
	output wire        bh,		// leg B high side
	output wire        bl,		// leg B low side
	output wire        en_a,	// leg A gate-driver enable (copy of en[0])
	output wire        en_b		// leg B gate-driver enable (copy of en[1])
);

	//------------------------------------------------------------------
	// Leg A : wired straight through。
	// mode_b = 3 时腿 A 被钉成"静态高"(AH 常开 / AL 常关), 由腿 B 按 duty 斩波。
	//------------------------------------------------------------------
	wire        mode_a_static = (mode_b == 2'd3);
	wire [31:0] duty_a_eff    = mode_a_static ? 32'hFFFFFFFF : duty;
	wire [15:0] dead_a_eff    = mode_a_static ? 16'd0        : dead_a;

	pwm_half_bridge u_leg_a
	(
		.clk	(clk),
		.rstn	(rstn),
		.run	(run),
		.inv	(inv),
		.en		(en[0]),
		.cnt	(cnt),
		.duty	(duty_a_eff),
		.dead	(dead_a_eff),
		.hi		(ah),
		.lo		(al)
	);

	//------------------------------------------------------------------
	// Leg B : outputs CROSS-CONNECTED so that BL is in phase with AH
	//         (the upper window drives BL, the lower window drives BH)
	//
	// mode_b selects what leg B actually does.  For an eddy-current brake /
	// damper coil the useful states are all reached by holding leg B STATIC
	// and PWM-ing leg A, exactly like the classic "PWM + direction" H-bridge
	// drive.  That is what makes low-ripple DC current control possible:
	//
	//   mode_b = 0  NORMAL (diagonal).  Leg B uses `duty` like leg A, so the
	//               diagonals conduct: V = Vbus*(2*duty/PERIOD - 1).
	//               This is the default and the motor-style drive.
	//
	//   mode_b = 1  Leg B STATIC LOW  -> BL on, BH off.
	//               Leg A PWMs: AH on  => load sees +Vbus
	//                          AL on  => load sees 0 V, freewheeling through
	//                                    the two low-side switches
	//               That freewheel is SLOW DECAY: the current stays smooth,
	//               ripple is Vbus*d*T/L instead of the 2*Vbus*d*(1-d)*T/L of
	//               the diagonal mode, and only leg A switches at all.
	//               With duty = PERIOD this gives clean DC with NO switching.
	//
	//   mode_b = 2  Leg B STATIC HIGH -> BH on, BL off.
	//               With leg A static low this puts -Vbus across the load, the
	//               standard way to force a coil current down FAST instead of
	//               waiting for the L/R decay.
	//
	//   mode_b = 3  Leg A STATIC HIGH + leg B PWM(本工程制动用的模式).
	//               腿 A 被钉成静态高(AH 常开、AL 常关), 腿 B 按 `duty`/`dead_b`
	//               互补斩波: BL 在 [dt_b, duty) 导通、BH 在 [duty+dt_b, PERIOD) 导通。
	//               负载 = duty/PERIOD * Vbus, 单向电流; 关断期经 AH+BH 续流(慢衰减)。
	//               注意腿 A 在 mode_b 传入前就被替换成 0xFFFFFFFF, 所以它的死区也
	//               强制为 0(静态腿不吃死区), 见下面的 duty_a_eff / dead_a_eff。
	//
	// A static leg MUST have zero dead time.  With dead_b != 0 the "static"
	// leg would drop out for dead_b cycles every period and the load would see
	// a reverse voltage spike instead of a clean DC level, so dead time is
	// forced to 0 here whenever a static mode is selected.
	//
	// Safety is unchanged: the two halves of a half bridge compare against the
	// same cnt and the same effective duty, and "hi requires cnt < duty" /
	// "lo requires cnt >= duty" stay mutually exclusive for 0, for 0xFFFFFFFF
	// and for every value in between.  A static leg cannot shoot through.
	//------------------------------------------------------------------
	wire        mode_static = (mode_b == 2'd1) || (mode_b == 2'd2);
	wire [31:0] duty_b_eff  = (mode_b == 2'd1) ? 32'hFFFFFFFF :	// static LOW
	                          (mode_b == 2'd2) ? 32'd0        :	// static HIGH
	                          duty;
	wire [15:0] dead_b_eff  = mode_static ? 16'd0 : dead_b;

	pwm_half_bridge u_leg_b
	(
		.clk	(clk),
		.rstn	(rstn),
		.run	(run),
		.inv	(inv),
		.en		(en[1]),
		.cnt	(cnt),
		.duty	(duty_b_eff),
		.dead	(dead_b_eff),
		.hi		(bl),
		.lo		(bh)
	);

	//------------------------------------------------------------------
	// Gate-driver enables: straight copies of the register bits.
	// Deliberately NOT gated by run, so the driver enable state and the
	// modulation enable can be controlled independently.
	//------------------------------------------------------------------
	assign en_a = en[0];
	assign en_b = en[1];

endmodule
