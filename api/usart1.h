#ifndef __USART1_
#define __USART1_

#include "stm32f4xx.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "debug.h"

void usart1_Config(uint32_t brr);
void usart1_tasks_init(void); // 建接收信号量 + 创建串口接收任务。调度器启动前调用

extern SemaphoreHandle_t usart_rx_sem; // 中断 → 任务的通知信号量:USART1_IRQHandler 给,usart_rx_task 取

#endif

