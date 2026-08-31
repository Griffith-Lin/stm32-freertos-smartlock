#include "led.h"

//pb8 pb9  io口电压为高时，熄灭
void led_ini(void)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB,ENABLE);
    
    GPIO_InitTypeDef gpio_InitTypeDef={0};
    
    gpio_InitTypeDef.GPIO_Mode=GPIO_Mode_OUT;
    gpio_InitTypeDef.GPIO_OType=GPIO_OType_PP;
    gpio_InitTypeDef.GPIO_Speed=GPIO_Low_Speed;
    gpio_InitTypeDef.GPIO_PuPd=GPIO_PuPd_NOPULL;
    gpio_InitTypeDef.GPIO_Pin=GPIO_Pin_8 | GPIO_Pin_9;
    
    
    //为什么要先置电平，再初始化gpio？这里是防止先初始化后，io口输出的是低电平，会先点亮led灯，后面再置电平就来不及了。
    GPIO_SetBits(GPIOB,GPIO_Pin_8);
    GPIO_SetBits(GPIOB,GPIO_Pin_9);
 
    GPIO_Init(GPIOB,&gpio_InitTypeDef);
    
    

    
//    GPIO_ResetBits(GPIOB,GPIO_Pin_8);
//    GPIO_ResetBits(GPIOB,GPIO_Pin_9);

       
}



