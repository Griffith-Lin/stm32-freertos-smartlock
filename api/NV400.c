#include "NV400.h"
#include "tim.h"        // TIM10->CNT:1us 一跳的硬件计数器,拿来做精确延时

/*******************
 * 函数功能：NV400F语音播报模块管脚初始化
 * 形参：无
 * 返回值：无
 * 函数描述：
    VOICE_BUSY------------PC4      //通用输入
    VOICE_DATA------------PC5      //通用推挽输出
*******************/
void NV400F_Init(void)
{
    //打开GPIOC时钟
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);

    // 先把输出锁存器(ODR)预置成高,再去改模式。
    // 反过来的话,GPIO_Init() 把 PC5 从输入切成输出的那一瞬间,引脚就按 ODR 的
    // 复位值(0)往下拉,线上会多出一个低电平毛刺。芯片会把这个毛刺当成起始信号,
    // 后面真发命令时它还在"收上一条"的状态里,自然不响。
    // 输入模式下写 ODR 不影响引脚电平,只是把锁存器备好,切过去直接就是高。
    VOICE_DATA_H;

    //初始化引脚为输出模式
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_5;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_4;
    GPIO_Init(GPIOC, &GPIO_InitStruct);
}


/**
 * @brief  微秒级忙等延时,用 TIM10 的计数器计时
 * @param  us 要延时多少微秒
 * @return 无
 * @note   为什么不用 delay.c 里的 Delay_Us() ?
 *         那个函数是数 NOP 空转,而且是**按 168MHz 标定的** —— 它自己的注释
 *         写着"198×5.95ns≈1178ns",5.95ns 正好是 1/168MHz 的周期。可本工程
 *         跑在 84MHz 的 F401 上,于是每个"微秒"实际是 2.06us。反汇编数出来:
 *         循环体 168 个 NOP + 5 周期循环开销 = 173 周期,173/84MHz = 2.06us,
 *         和推算完全对得上。
 *         要命的是本协议不只看高低电平的**比例**,对绝对宽度也有要求
 *         (手册推荐 100:300 ~ 400:1200us)。比例虽然是 3:1 没错,但位宽
 *         整体翻倍成 824us/2471us,超出范围,芯片直接解不出来 ——
 *         现象就是"烧录成功、一声不响"。
 * @note   换成硬件计数器之后:不受主频、优化等级、Flash 等待周期影响,
 *         中断插进来也不会算错 —— 它量的是真实流逝的时间,而不是数了多少条指令。
 * @note   TIM10_Task_Config(84, 1000) 已经把 CNT 配成 1us 一跳、数到 999 归零,
 *         所以这里按"模 1000"累计,不会因为计数器溢出而少算。
 */
void NV400F_DelayUs(uint32_t us)
{
    uint32_t prev = TIM10->CNT;
    uint32_t acc  = 0;

    while (acc < us)
    {
        uint32_t now  = TIM10->CNT;
        // now < prev 说明两次读之间计数器绕过了一次 0,
        // 这一段实际经过了 (1000 - prev) + now 微秒
        uint32_t step = (now >= prev) ? (now - prev) : (1000u + now - prev);

        acc += step;
        prev = now;
    }
}


/*******************
 * 函数功能：NV400F语音播报模块发送8bit协议码
 * 形参：u8 cmd     发送8bit协议码
 * 返回值：无
 * 函数描述：
    一次发送8bit数据且低位先出
*******************/
void NV400F_SendCmd(u8 cmd)
{
    //起始信号:数据线拉低 5ms(手册给的范围是 3~9ms,取中间值)
    VOICE_DATA_L;
    NV400F_DelayUs(5000);

    //发送8bit数据
    for(u8 i=0;i<8;i++)
    {
        if(cmd & 0x01 << i)
        {
            //数据'1'  ---  1码:高 1200us + 低 400us(3:1)
            VOICE_DATA_H;
            NV400F_DelayUs(1200);
            VOICE_DATA_L;
            NV400F_DelayUs(400);
        }
        else
        {
            //数据'0'  ---  0码:高 400us + 低 1200us(1:3)
            VOICE_DATA_H;
            NV400F_DelayUs(400);
            VOICE_DATA_L;
            NV400F_DelayUs(1200);
        }
    }

    //收尾:回到空闲高电平。最后一位如果正好是"低 1200us"那段,这里就是它的结束沿;
    //之后连着的下一个命令起始低电平,中间靠这行保证至少有个高电平间隔。
    VOICE_DATA_H;
}
