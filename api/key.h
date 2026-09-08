#ifndef _KEY_
#define _KEY_

#include "stm32f4xx.h"
#include "led.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#define KEY_EVENT_NONE        0   // 无事件
#define KEY_EVENT_PRESS       1   // 按下确认(进入PRESSED瞬间)
#define KEY_EVENT_SHORT_PRESS 2   // 短按(释放时判定)
#define KEY_EVENT_LONG_PRESS  3   // 长按(释放时判定)
#define LONG_PRESS_THRESHOLD  600 // 长按阈值,单位ms

extern uint8_t flag;      // 长按演示标志:key_action_task 置位,主循环观察

typedef enum
{
    KEY_STATE_IDLE,     // 空闲
    KEY_STATE_DEBOUNCE, // 消抖确认中
    KEY_STATE_PRESSED,  // 已确认按下
    KEY_STATE_RELEASE   // 释放检测中
} KeyState_t;

typedef struct
{
    GPIO_TypeDef *port;          // 按键所在 GPIO 端口,如 GPIOB
    uint16_t pin;
    KeyState_t state;            // 状态机当前状态
    uint8_t debounce_count;      // 同一电平连续扫描次数(10ms扫描下 >=3 = 30ms消抖)
    uint32_t press_start_time;   // 按下确认时刻(xTaskGetTickCount,1ms/拍),用于算按压时长
    uint8_t event_flag;          // 事件暂存信箱:scan 任务取走 → 塞进队列
} Key_t;

void key_Config(void);
void key_tasks_init(void);       // 建队列 + 配按键GPIO + 创建 scan/action 两个任务
extern QueueHandle_t key_queue;  // 按键事件队列(任务间通信):scan 生产 → action 消费

#endif
