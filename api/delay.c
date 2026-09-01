#include "delay.h"


void Delay_nop(u32 num)
{
    num/=4;
    while(num--)
    {
        __NOP();
    }
}

/*

你定义的 void Delay_nop(u32 num) 即使被编译器内联（Inline），其 while(num--) 循环在 ARM Cortex-M4 架构下，每次迭代的汇编指令大致如下：
loop:
    subs r0, r0, #1  ; num-- (1 周期)
    nop              ; __NOP() (1 周期)
    cmp r0, #0       ; 判断是否等于0 (1 周期)
    bne loop         ; 不等于0则跳转回 loop (1~3 周期)


单次迭代耗时：约 4 ~ 5 个周期。
44次迭代总耗时：
44×4.5≈198 个周期。
实际时间：198×5.95ns≈1178ns （即 1.18us）。
致命后果：如果你用它来代替 '0' 码的高电平延时，原本应该是 300ns 的高电平被拉长到了 1.18us。WS2812 会将其误判为 '1' 码，导致整条灯带颜色全乱。

*/

void Delay_Us(u32 ustime)
{
	while(ustime--)
	{
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
		__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();__NOP();
	}
}


void Delay_Ms(u32 mstime)
{
	while(mstime--)
	{
		Delay_Us(1000);
	}
}




