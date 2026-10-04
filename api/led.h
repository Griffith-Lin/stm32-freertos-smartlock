#ifndef __LED_
#define __LED_

#include "stm32f4xx.h"

/* 为什么用 do { ... } while (0) 包起来:
 * 宏体里直接在末尾写分号,那条分号就成了宏的一部分 —— 展开后多出一条空语句,
 * 会把 if 和 else 隔开:
 *     if (x) LED_RED_ON();          →  if (x) GPIO_ResetBits(...);;
 *     else   LED_GREEN_ON();        →  else   GPIO_SetBits(...);;
 * 编译报 #127: expected a statement,指着 else 那一行。
 * 套上 do{}while(0) 后,整条宏在语法上就等于"一条语句",能安全跟在 if/else 后面,
 * 而且仍然要求调用者写分号 —— 用起来和普通函数一模一样。
 * (注意:调用处必须带分号,LED_RED_ON() 后面别漏) */
#define LED_RED_ON()       do { GPIO_ResetBits(GPIOB, GPIO_Pin_8); } while (0) // 红灯亮(低电平点亮)
#define LED_GREEN_ON()     do { GPIO_ResetBits(GPIOB, GPIO_Pin_9); } while (0) // 绿灯亮(低电平点亮)

#define LED_RED_OFF()      do { GPIO_SetBits(GPIOB, GPIO_Pin_8); } while (0) // 红灯灭
#define LED_GREEN_OFF()    do { GPIO_SetBits(GPIOB, GPIO_Pin_9); } while (0) // 绿灯灭

#define LED_RED_toggle()   do { GPIO_ToggleBits(GPIOB, GPIO_Pin_8); } while (0) // 红灯翻转
#define LED_GREEN_toggle() do { GPIO_ToggleBits(GPIOB, GPIO_Pin_9); } while (0) // 绿灯翻转

void led_Config(void);

#endif