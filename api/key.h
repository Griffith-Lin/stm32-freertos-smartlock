#ifndef _KEY_
#define _KEY_

#include "stm32f4xx.h"
#include "delay.h"
#include "tim.h"

#define KEY_EVENT_NONE 0          // 无事件
#define KEY_EVENT_PRESS 1         // 按下确认(进入PRESSED瞬间)
#define KEY_EVENT_SHORT_PRESS 2   // 短按(释放时判定)
#define KEY_EVENT_LONG_PRESS 3    // 长按(释放时判定)
#define LONG_PRESS_THRESHOLD 600 // 长按阈值,单位ms


extern volatile uint8_t flag;

typedef enum
{
    KEY_STATE_IDLE,     // 空闲
    KEY_STATE_DEBOUNCE, // 消抖确认中
    KEY_STATE_PRESSED,  // 已确认按下
    KEY_STATE_RELEASE   // 释放检测中
} KeyState_t;

typedef struct
{
    GPIO_TypeDef *port; // 按键所在 GPIO 端口,如 GPIOB
    uint16_t pin;
    KeyState_t state;          // 状态机当前状态: IDLE(空闲)/DEBOUNCE(消抖)/PRESSED(按下)/RELEASE(释放)
    uint8_t debounce_count;    // 消抖计数: 同一电平连续扫描次数,>=3(=30ms)才确认有效
    uint32_t press_start_time; // 按下确认时刻的毫秒计数(GetSystemTick返回值),用于算按压时长
    uint8_t event_flag;        // 事件标志(生产者-消费者信箱): 中断置位,主循环消费后清0
}Key_t;

    extern volatile uint32_t motor_ccr;

    void key_scan_tim_ini(void);

    void key_ini(void);
    void Exti_key_ini(void);
#endif
