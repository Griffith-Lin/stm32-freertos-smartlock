#ifndef __SOFT_TIMER_
#define __SOFT_TIMER_

#include "stm32f4xx.h"

#include "FreeRTOS.h"
#include "timers.h"

void soft_timer_demo_init(void); // 建两个演示用软件定时器并启动。调度器启动前调用

#endif
