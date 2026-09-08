#ifndef _DELAY_H_
#define _DELAY_H_

#include "stm32f4xx.h"

void Delay_Us(u32 ustime);
void Delay_Ms(u32 mstime);
void Delay_nop(u32 num);

#endif
