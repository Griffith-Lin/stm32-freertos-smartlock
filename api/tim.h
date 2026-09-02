#ifndef __TIM_
#define __TIM_

#include "stm32f4xx.h"
#include "stdio.h" //ø…“‘”√printf


#include <stddef.h>
extern volatile uint32_t tim10_tick_ms;

void TIM10_delay(uint32_t ms);
void TIM10_Task_Init(uint32_t psc,uint32_t arr);
void in_cap_ini(uint16_t psc,uint16_t arr);
uint32_t GetTim10Tick(void);

#endif
