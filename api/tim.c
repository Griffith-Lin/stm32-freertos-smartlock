#include "tim.h"

// apb2预分频系数为1，定时器不倍频。84MHz,分频系数84，实际频率1MHz，一次1us，arr为1000，周期为1ms
void TIM10_Task_Config(uint32_t psc, uint32_t arr)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM10, ENABLE);

    TIM_TimeBaseInitTypeDef tim_TimeBaseInitTypeDef; // 时基结构体

    tim_TimeBaseInitTypeDef.TIM_Period = arr - 1;                 // 对应 ARR
    tim_TimeBaseInitTypeDef.TIM_Prescaler = psc - 1;              // 对应 PSC
    tim_TimeBaseInitTypeDef.TIM_ClockDivision = TIM_CKD_DIV1;     // TIM10 忽略此项
    tim_TimeBaseInitTypeDef.TIM_CounterMode = TIM_CounterMode_Up; // TIM10 只能向上计数，忽略此项

    TIM_TimeBaseInit(TIM10, &tim_TimeBaseInitTypeDef);

    // 禁能自动重载预装载 (ARPE=0)
    TIM_ARRPreloadConfig(TIM10, DISABLE);

    // 使能更新中断
    TIM_ITConfig(TIM10, TIM_IT_Update, ENABLE);

    // 手动生成更新事件
    //  作用：将当前设置的 PSC 和 ARR 立即更新到影子寄存器，确保首次启动时参数生效
    TIM_GenerateEvent(TIM10, TIM_EventSource_Update);

    TIM_ClearFlag(TIM10, TIM_FLAG_Update); // 如果不先清掉UIF，每次上电中断使能瞬间会白多触发一次更新中断

    NVIC_InitTypeDef NVIC_InitStructure;

    NVIC_InitStructure.NVIC_IRQChannel = TIM1_UP_TIM10_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1; // 抢占优先级
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;        // 次优先级

    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;

    NVIC_Init(&NVIC_InitStructure);

    TIM_Cmd(TIM10, ENABLE); // 在延时函数里使能和禁能，能减小能耗。这里tim10作为时基，不能停
}

volatile uint32_t tim10_tick_ms = 0;
void TIM10_delay(uint32_t ms)
{
    uint32_t start = tim10_tick_ms;
    while (tim10_tick_ms - start < ms)
        ; // 无符号减法，2^32ms(约49.7天)回绕也正确
}

uint32_t GetTim10Tick(void)
{
    return tim10_tick_ms; // TIM10 1ms中断维护的毫秒计数
}

// TIM10 更新中断:只维护毫秒时基(服务 L9110S_move 等)
// 按键扫描已搬到 FreeRTOS 的 key_scan_task(每10ms一次),不再进中断
void TIM1_UP_TIM10_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM10, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM10, TIM_IT_Update);
        tim10_tick_ms++; // 时基,纯节拍维护
    }
}
