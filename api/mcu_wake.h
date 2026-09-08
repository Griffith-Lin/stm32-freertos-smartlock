#ifndef __MCU_WAKE_
#define __MCU_WAKE_

#include "stm32f4xx.h"

// mcu_wkup(PA0):STM32F401 硬件 WKUP 引脚,STANDBY 唤醒专用(上升沿唤醒)。
// 待机时调度器/任务全部冻结,唤醒 = 整片复位 —— 本模块全部工作在 FreeRTOS 之外。

void mcu_wkup_Config(void);          // 使能 PWR 时钟 + PA0 输入(NOPULL,空闲低靠外部下拉)

uint8_t mcu_was_standby_wake(void);  // 开机后调用:返回1 = 本次复位来自 PA0 待机唤醒(顺带清标志)

void mcu_enter_standby(void);        // 进 STANDBY:PA0 上升沿唤醒,醒来即复位,本函数不返回

#endif
