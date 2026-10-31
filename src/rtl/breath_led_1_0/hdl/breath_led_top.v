module breath_led_top
#(
	parameter	BREATH_FREQ	=	10'd100
)
(
	input	wire			sys_clk			,	//系统时钟
	input	wire			rst_n			,	//复位
		
	//呼吸灯控制参数	
	input	wire			breath_led_en	,	//呼吸灯使能信号 1:打开 0:关闭
	input	wire	[9:0]	freq_set		,	//呼吸频率设置 10-1000

	output	wire			led					//输出led

);
//*****************************************************
//**                  reg define
//*****************************************************
reg	[15:0]	cnt_1ms			;	//1ms计数器
reg	[9:0]	freq_set_reg	;	//呼吸灯频率寄存
reg	[15:0]	high_duty_cnt	;	//高电平计数
reg			duyt_flag		;	//1表示占空比减少 0表示占空比增加
//*****************************************************
//**                  assign code
//*****************************************************
//当呼吸灯使能才与占空比进行比较 输出pwm  否则直接输出0
assign	led	=	(!breath_led_en)?1'b0:(cnt_1ms <= high_duty_cnt)?1'b1:1'b0;
//*****************************************************
//**                  always code
//*****************************************************
//周期计数 生成pwm波的周期 1ms 50_000
always @ (posedge sys_clk	or negedge rst_n) begin
    if (!rst_n)
        cnt_1ms <= 16'd0;
    else if(!breath_led_en)
        cnt_1ms <= 16'd0;
    else if( cnt_1ms == (50000-1) )
        cnt_1ms <= 16'd0;
    else
        cnt_1ms <= cnt_1ms + 16'd1;
end

//改变呼吸灯频率 最小10 最大1000
always @ (posedge sys_clk	or negedge rst_n) begin
    if (!rst_n)
		freq_set_reg	<=	BREATH_FREQ;
	else	if(freq_set >= 1000)
		freq_set_reg	<=	1000;
	else	if(freq_set <= 10)
		freq_set_reg	<=	10	;
	else
		freq_set_reg	<=	freq_set;
end

//生成pwm波
always @ (posedge sys_clk	or negedge rst_n) begin
    if (!rst_n)
	begin
		high_duty_cnt	<=	16'd0	;
		duyt_flag		<=	1'b0	;
	end
	else	if(breath_led_en==0)	//未使能呼吸灯 全部置0
	begin
		high_duty_cnt	<=	16'd0	;
		duyt_flag		<=	1'b0	;
	end
	else	if(cnt_1ms == 16'd50_000-1)	//计数完成一个周期
	begin
		if(duyt_flag)	//调节占空比的值和方向 1表示减  
		begin
			if(high_duty_cnt == 16'd0)
				duyt_flag	<=	1'b0;
			else	if(high_duty_cnt < freq_set_reg)
				high_duty_cnt	<=	16'd0;
			else
				high_duty_cnt	<=	high_duty_cnt - freq_set_reg;
		end
		else	//占空比增加
		begin
			if( high_duty_cnt >= 16'd50_000-1)
				duyt_flag	<=	1'b1;
			else
				high_duty_cnt	<=	high_duty_cnt + freq_set_reg;
		end
	end
	else	//一个周期没到 就保持占空比不变
		high_duty_cnt	<=	high_duty_cnt;
end

	
endmodule