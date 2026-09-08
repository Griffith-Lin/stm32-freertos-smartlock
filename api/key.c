#include "key.h"

volatile Key_t keys[3] = {0};

// pc13 key2 输入模式，当出现从高到低电平的跳变时，触发中断
void key_Config(void)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);

    GPIO_InitTypeDef gpio_InitTypeDef = {0};

    gpio_InitTypeDef.GPIO_Mode = GPIO_Mode_IN;
    gpio_InitTypeDef.GPIO_PuPd = GPIO_PuPd_UP;
    gpio_InitTypeDef.GPIO_Pin = GPIO_Pin_13;

    GPIO_SetBits(GPIOC, GPIO_Pin_13);

    GPIO_Init(GPIOC, &gpio_InitTypeDef);

    keys[1].port = GPIOC;
    keys[1].pin = GPIO_Pin_13;
    keys[1].state = KEY_STATE_IDLE;
    keys[1].event_flag = KEY_EVENT_NONE;
}

void Exti_key_ini(void)
{
    key_Config();

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);

    SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOC, EXTI_PinSource13);

    EXTI_InitTypeDef exti_InitTypeDef = {0};

    // EXTI_Mode_Interrupt
    // 该EXTI线配置为中断模式，触发后进入NVIC中断服务程序
    // EXTI_Mode_Event
    // 该EXTI线配置为事件模式，触发后产生脉冲信号，可唤醒CPU但不进ISR，常用于触发ADC/DMA等外设
    exti_InitTypeDef.EXTI_Mode = EXTI_Mode_Interrupt;
    exti_InitTypeDef.EXTI_Line = EXTI_Line13;
    exti_InitTypeDef.EXTI_Trigger = EXTI_Trigger_Falling; // 下降沿触发
    exti_InitTypeDef.EXTI_LineCmd = ENABLE;

    EXTI_Init(&exti_InitTypeDef);

    NVIC_InitTypeDef nvic_InitTypeDef = {0};

    nvic_InitTypeDef.NVIC_IRQChannel = EXTI15_10_IRQn;
    nvic_InitTypeDef.NVIC_IRQChannelSubPriority = 0;
    nvic_InitTypeDef.NVIC_IRQChannelPreemptionPriority = 1;
    nvic_InitTypeDef.NVIC_IRQChannelCmd = ENABLE;

    NVIC_Init(&nvic_InitTypeDef);
}

void ProcessKey(volatile Key_t *key)
{
    uint8_t level = GPIO_ReadInputDataBit(key->port, key->pin);

    switch (key->state)
    {
    case KEY_STATE_IDLE:
        if (level == 0) // 问的是：按下了吗？
        {
            key->state = KEY_STATE_DEBOUNCE;
            key->debounce_count = 0;
        }
        break;

    case KEY_STATE_DEBOUNCE: // 按下消抖
        if (level == 0)      // 问的是：按下了吗？
        {
            if (++key->debounce_count >= 3) // 连续 3 次 ==0 ?	3ms 连续低 确认真的按住了 → PRESSED
            {
                key->state = KEY_STATE_PRESSED;
                key->press_start_time = GetTim10Tick();
                key->event_flag = KEY_EVENT_PRESS;
            }
        }
        else
        {
            key->state = KEY_STATE_IDLE; // 判定为抖动,作废，回空闲状态
            key->debounce_count = 0;
        }
        break;

    case KEY_STATE_PRESSED: // 已确认按下,检测释放
        if (level == 1)     // 问的是：松开了吗？
        {
            key->debounce_count = 0;
            key->state = KEY_STATE_RELEASE; // 先进释放消抖,别直接判事件
        }
        break;

    case KEY_STATE_RELEASE: // 释放消抖:连续3次为高才确认释放
        if (level == 1)     // 问的是：松开了吗？
        {
            if (++key->debounce_count >= 3) // 连续三次判定成功，进入长短按时长判断
            {
                uint32_t duration = GetTim10Tick() - key->press_start_time;
                key->event_flag = (duration > LONG_PRESS_THRESHOLD)
                                      ? KEY_EVENT_LONG_PRESS
                                      : KEY_EVENT_SHORT_PRESS;
                key->state = KEY_STATE_IDLE;
            }
        }
        else // 释放消抖中又变低：没松干净，回到 PRESSED 重新检测释放
        {
            key->state = KEY_STATE_PRESSED;
            key->debounce_count = 0;
        }
        break;
    }
}

volatile uint8_t flag = 0;

void TIM1_UP_TIM10_IRQHandler(void)
{
    // LED_RED_toggle();

    if (TIM_GetITStatus(TIM10, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM10, TIM_IT_Update);
        tim10_tick_ms++; // 时基，纯节拍维护
        ProcessKey(&keys[1]);
    }

    if (keys[1].event_flag == KEY_EVENT_SHORT_PRESS) // 短按事件
    {
        keys[1].event_flag = KEY_EVENT_NONE;
        LED_RED_toggle();
    }

    if (keys[1].event_flag == KEY_EVENT_LONG_PRESS) // 长按事件
    {
        keys[1].event_flag = KEY_EVENT_NONE; // 消费事件
        LED_RED_toggle();
        flag = 1;
    }
}
