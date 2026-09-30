#ifndef __DEBUG_
#define __DEBUG_

#include "stm32f4xx.h"
#include "dma.h"

#include "FreeRTOS.h"
#include "semphr.h"

void debug_init(void);          // 建打印锁。必须在 vTaskStartScheduler() 之前调用
void print_line(const char *s); // 整行原子打印。串口是全局共享资源,所有任务都要走这里
void die(uint16_t n);

#endif
