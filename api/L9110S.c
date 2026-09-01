#include "L9110S.h"

//X4  X3负责直接驱动电机，外接引脚
//BAK FOR负责方向 pb3 pb4
//当BAK FOR都为0时，x4 x3高阻态
void L9110S_ini(void)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB,ENABLE);
    
    GPIO_InitTypeDef gpio_InitTypeDef={0};
    
    gpio_InitTypeDef.GPIO_Mode=GPIO_Mode_OUT;
    gpio_InitTypeDef.GPIO_OType=GPIO_OType_PP;
    gpio_InitTypeDef.GPIO_Speed=GPIO_Low_Speed;
    gpio_InitTypeDef.GPIO_PuPd=GPIO_PuPd_NOPULL;
    gpio_InitTypeDef.GPIO_Pin=GPIO_Pin_3 | GPIO_Pin_4;
 
    GPIO_Init(GPIOB,&gpio_InitTypeDef);

   
}

void L9110S_control(void)
{
    GPIO_SetBits(GPIOB,GPIO_Pin_3);
    
}

