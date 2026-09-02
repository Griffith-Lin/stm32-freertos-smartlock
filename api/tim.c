#include "tim.h"


//apb2预分频系数为1，定时器不倍频。84MHz,分频系数84，实际频率1MHz，一次1us，arr为1000，周期为1ms
void TIM10_Task_Init(uint32_t psc,uint32_t arr) 
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM10,ENABLE);
    
    TIM_TimeBaseInitTypeDef tim_TimeBaseInitTypeDef;//时基结构体
    
    

    tim_TimeBaseInitTypeDef.TIM_Period = arr-1;                  // 对应 ARR
    tim_TimeBaseInitTypeDef.TIM_Prescaler = psc-1;               // 对应 PSC
    tim_TimeBaseInitTypeDef.TIM_ClockDivision = TIM_CKD_DIV1;  // TIM10 忽略此项
    tim_TimeBaseInitTypeDef.TIM_CounterMode = TIM_CounterMode_Up; // TIM10 只能向上计数，忽略此项
    
    TIM_TimeBaseInit(TIM10, &tim_TimeBaseInitTypeDef);

    // 禁能自动重载预装载 (ARPE=0)    
    TIM_ARRPreloadConfig(TIM10, DISABLE);

    //使能更新中断 
    TIM_ITConfig(TIM10, TIM_IT_Update, ENABLE);

    //手动生成更新事件 
    // 作用：将当前设置的 PSC 和 ARR 立即更新到影子寄存器，确保首次启动时参数生效
    TIM_GenerateEvent(TIM10, TIM_EventSource_Update);

    TIM_ClearFlag(TIM10,TIM_FLAG_Update);//如果不先清掉UIF，每次上电中断使能瞬间会白多触发一次更新中断
    
    NVIC_InitTypeDef NVIC_InitStructure;
    
    NVIC_InitStructure.NVIC_IRQChannel=TIM1_UP_TIM10_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority=1;//抢占优先级
    NVIC_InitStructure.NVIC_IRQChannelSubPriority=0;//次优先级
    
    NVIC_InitStructure.NVIC_IRQChannelCmd=ENABLE;
    
    NVIC_Init(&NVIC_InitStructure);
    
    
    TIM_Cmd(TIM10, ENABLE);//在延时函数里使能和禁能，能减小能耗。这里tim10作为时基，不能停
}


//volatile uint32_t tim10=0;

//TIM_Cmd(DISABLE) 停表时，计数器冻结在某个非零值 k（比如 437）。
//下次 TIM_Cmd(ENABLE) 从 437 继续数，而不是从 0 开始——第一次更新事件只需要 1000−437 个计数就来了，也就是说每次延时都比设定值短 0~1ms。延时次数多了误差会叠加。
// void TIM10_delay(uint32_t ms)
// {
//     TIM_Cmd(TIM10,ENABLE);
    
//     tim10=ms;
//     while(tim10);    
    
//     TIM_Cmd(TIM10,DISABLE);
// }
volatile uint32_t tim10_tick_ms = 0;
void TIM10_delay(uint32_t ms)
{
    uint32_t start = tim10_tick_ms;
    while (tim10_tick_ms - start < ms);   // 无符号减法，2^32ms(约49.7天)回绕也正确
}



// volatile uint32_t last_touch_ms = 0;      

//中断外面用TIM_GetFlagStatus,中断里面用TIM_GetITStatus
// void TIM1_UP_TIM10_IRQHandler(void)
// {
//     if(TIM_GetITStatus(TIM10, TIM_IT_Update)==SET)
//     {
//         TIM_ClearITPendingBit(TIM10, TIM_IT_Update);
//         tim10_tick_ms++;
        
        
//         if (tim10 > 0) 
//         {tim10--;}
//     }
// }







uint32_t GetTim10Tick(void)
{
    return tim10_tick_ms;   // TIM10 1ms中断维护的毫秒计数
}




// //pb0配置为输入捕获模式
// void in_cap_ini(uint16_t psc,uint16_t arr)
// {
//     RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB,ENABLE);
//     RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3,ENABLE);
    
//     GPIO_InitTypeDef gpio_InitTypeDef={0};
    
//     gpio_InitTypeDef.GPIO_Mode=GPIO_Mode_AF;
//     gpio_InitTypeDef.GPIO_PuPd=GPIO_PuPd_NOPULL;
//     gpio_InitTypeDef.GPIO_Pin=GPIO_Pin_0;
    
//     GPIO_PinAFConfig(GPIOB,GPIO_PinSource0,GPIO_AF_TIM3);
    
//     GPIO_Init(GPIOB,&gpio_InitTypeDef);
    
   
    
    
//     TIM_TimeBaseInitTypeDef tim_TimeBaseInitTypeDef={0};
    
//     tim_TimeBaseInitTypeDef.TIM_CounterMode=TIM_CounterMode_Up;
//     tim_TimeBaseInitTypeDef.TIM_ClockDivision=TIM_CKD_DIV1;
//     tim_TimeBaseInitTypeDef.TIM_Period=arr-1;
//     tim_TimeBaseInitTypeDef.TIM_Prescaler=psc-1;
    
//     TIM_TimeBaseInit(TIM3,&tim_TimeBaseInitTypeDef);
    
    
    
    
//     TIM_ICInitTypeDef tim_ICInitTypeDef={0};
    
//     tim_ICInitTypeDef.TIM_Channel=TIM_Channel_3;//TIM3的通道3
//     tim_ICInitTypeDef.TIM_ICPrescaler=TIM_ICPSC_DIV1;
//     tim_ICInitTypeDef.TIM_ICSelection=TIM_ICSelection_DirectTI;//TIM1是否直连IC1
//     tim_ICInitTypeDef.TIM_ICFilter=0;//过滤不管它
//     tim_ICInitTypeDef.TIM_ICPolarity=TIM_ICPolarity_BothEdge;//输入捕获的极性  上升沿捕获还是下降沿捕获，还是上下都捕获
       
//     TIM_ICInit(TIM3,&tim_ICInitTypeDef);

         
//     NVIC_InitTypeDef nvic_InitTypeDef={0};
    
//     nvic_InitTypeDef.NVIC_IRQChannel=TIM3_IRQn;//配置的是tim3的中断
//     nvic_InitTypeDef.NVIC_IRQChannelPreemptionPriority=3;
//     nvic_InitTypeDef.NVIC_IRQChannelSubPriority=0;
//     nvic_InitTypeDef.NVIC_IRQChannelCmd=ENABLE;
    
//     NVIC_Init(&nvic_InitTypeDef);
    
//     //EXTI是外部中断，而TIM中断是定时器内部事件中断。所以并不用配 EXTI中断
    
//     TIM_ARRPreloadConfig(TIM3, DISABLE);
//     TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);
//     TIM_GenerateEvent(TIM3, TIM_EventSource_Update);  
//     TIM_ITConfig(TIM3, TIM_IT_Update | TIM_IT_CC3, ENABLE); //使能中断 (同时使能 更新中断 和 通道3捕获中断)
    
    
    
//     TIM_Cmd(TIM3,ENABLE);
// }

// uint16_t tim3_begin=0;
// uint16_t tim3_count=0;
// uint16_t Tsum=0;
// uint8_t tim3_flag=0;


// //SetSysClock() 函数里 RCC_CFGR_PPRE1_DIVx 就是APB1 总线预分频器 的分频系数（AHB 时钟 HCLK 对 APB1 时钟 PCLK1 的分频系数）。你的是 DIV4
// //psc==84  arr==1000,   基准频率==1MHz  Tcnt==1us  Tperiod==1*1000==1ms
// //按键触发后为低电平

// void TIM3_IRQHandler(void)
// {
//     //更新中断标志位处理
//     if(TIM_GetITStatus(TIM3,TIM_IT_Update))
//     {
//         TIM_ClearITPendingBit(TIM3,TIM_IT_Update);
        
//         if(tim3_flag==1)
//         tim3_count++;
//     }
    
  
//     //捕获中断标志位处理
//     if(TIM_GetITStatus(TIM3,TIM_IT_CC3))
//     {
//        TIM_ClearITPendingBit(TIM3,TIM_IT_CC3);
        
//         if(GPIO_ReadInputDataBit(GPIOB,GPIO_Pin_0)==0)
//         {
//             tim3_flag=1;           
//             tim3_begin=TIM_GetCapture3(TIM3);
//             tim3_count=0;
            
//         }
//         else
//         {           
//             if(tim3_flag==1)
//             {
//             tim3_flag=0;
//             Tsum=(tim3_count*1000-tim3_begin+TIM_GetCapture3(TIM3))/1000;//除1000变成毫秒
            
//             if(Tsum>50)
//             printf("按下时长为%dms\r\n",Tsum);
            
//             if(Tsum>1000)
//             {
//                 status_dev.PlayState=PLAY_NEXT;
//             }
            
//             tim3_begin=0;
//             tim3_count=0;
//             Tsum=0;
//             }
//         }
        
        
        
//     }
// }    

