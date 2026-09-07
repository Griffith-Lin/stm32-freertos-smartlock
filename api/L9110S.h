#ifndef __L9110S_
#define __L9110S_

#include "stm32f4xx.h"
#include "delay.h"
#include "tim.h"

typedef enum {
    MOTOR_FREE,     // 停止，高阻自由滑行
    MOTOR_FORWARD,  // 正转
    MOTOR_REVERSE,  // 反转
    MOTOR_BRAKE     // 刹车
} L9110S_Mode_t;

void L9110S_control(L9110S_Mode_t mode);
void L9110S_move(L9110S_Mode_t mode, uint32_t ms);   // 定时动作


void L9110S_Config(void);
#endif

