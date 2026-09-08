#ifndef __LED_
#define __LED_

#include "stm32f4xx.h"

#define LED_RED_ON()       GPIO_ResetBits(GPIOB, GPIO_Pin_8); // 红灯
#define LED_BLUE_ON()     GPIO_ResetBits(GPIOB, GPIO_Pin_9); // 绿灯

#define LED_RED_OFF()      GPIO_SetBits(GPIOB, GPIO_Pin_8); // 红灯
#define LED_BLUE_OFF()    GPIO_SetBits(GPIOB, GPIO_Pin_9); // 绿灯

#define LED_RED_toggle()   GPIO_ToggleBits(GPIOB, GPIO_Pin_8); // 红灯翻转
#define LED_BLUE_toggle() GPIO_ToggleBits(GPIOB, GPIO_Pin_9); // 绿灯翻转

void led_Config(void);

#endif