module breath_led_top
#(
	parameter	BREATH_FREQ	=	10'd100
)
(
	input	wire			sys_clk			,	//ϵͳʱ��
	input	wire			rst_n			,	//��λ
		
	//�����ƿ��Ʋ���	
	input	wire			breath_led_en	,	//������ʹ���ź� 1:�� 0:�ر�
	input	wire	[9:0]	freq_set		,	//����Ƶ������ 10-1000

	output	wire			led					//���led

);
//*****************************************************
//**                  reg define
//*****************************************************
reg	[15:0]	cnt_1ms			;	//1ms������
reg	[9:0]	freq_set_reg	;	//������Ƶ�ʼĴ�
reg	[15:0]	high_duty_cnt	;	//�ߵ�ƽ����
reg			duyt_flag		;	//1��ʾռ�ձȼ��� 0��ʾռ�ձ�����
//*****************************************************
//**                  assign code
//*****************************************************
//��������ʹ�ܲ���ռ�ձȽ��бȽ� ���pwm  ����ֱ�����0
assign	led	=	(!breath_led_en)?1'b0:(cnt_1ms <= high_duty_cnt)?1'b1:1'b0;
//*****************************************************
//**                  always code
//*****************************************************
//���ڼ��� ����pwm�������� 1ms 50_000
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

//�ı������Ƶ�� ��С10 ���1000
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

//����pwm��
always @ (posedge sys_clk	or negedge rst_n) begin
    if (!rst_n)
	begin
		high_duty_cnt	<=	16'd0	;
		duyt_flag		<=	1'b0	;
	end
	else	if(breath_led_en==0)	//δʹ�ܺ����� ȫ����0
	begin
		high_duty_cnt	<=	16'd0	;
		duyt_flag		<=	1'b0	;
	end
	else	if(cnt_1ms == 16'd50_000-1)	//�������һ������
	begin
		if(duyt_flag)	//����ռ�ձȵ�ֵ�ͷ��� 1��ʾ��  
		begin
			if(high_duty_cnt == 16'd0)
				duyt_flag	<=	1'b0;
			else	if(high_duty_cnt < freq_set_reg)
				high_duty_cnt	<=	16'd0;
			else
				high_duty_cnt	<=	high_duty_cnt - freq_set_reg;
		end
		else	//ռ�ձ�����
		begin
			if( high_duty_cnt >= 16'd50_000-1)
				duyt_flag	<=	1'b1;
			else
				high_duty_cnt	<=	high_duty_cnt + freq_set_reg;
		end
	end
	else	//һ������û�� �ͱ���ռ�ձȲ���
		high_duty_cnt	<=	high_duty_cnt;
end

	
endmodule