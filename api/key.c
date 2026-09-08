#include "key.h"

volatile Key_t keys[3] = {0};
uint8_t flag = 0;        // 长按置位(由 key_action_task 置,供主循环观察)
QueueHandle_t key_queue = NULL;   // 事件队列:key_scan_task 生产, key_action_task 消费

// pc13 key2:输入模式+上拉,按下接地(低有效)
void key_Config(void)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);

    GPIO_InitTypeDef gpio_InitTypeDef = {0};
    gpio_InitTypeDef.GPIO_Mode = GPIO_Mode_IN;
    gpio_InitTypeDef.GPIO_PuPd = GPIO_PuPd_UP;
    gpio_InitTypeDef.GPIO_Pin = GPIO_Pin_13;

    GPIO_Init(GPIOC, &gpio_InitTypeDef);

    keys[1].port = GPIOC;
    keys[1].pin = GPIO_Pin_13;
    keys[1].state = KEY_STATE_IDLE;
    keys[1].event_flag = KEY_EVENT_NONE;
}

/**
 * @brief 按键状态机(纯逻辑,不知道也不关心谁调用它)
 * @param key 按键对象指针
 * @note 现在由 key_scan_task 每 10ms 调用一次(任务上下文,不是中断!)
 *       debounce_count>=3 即 30ms 消抖
 *       时间一律用 FreeRTOS 内核时基 xTaskGetTickCount()(1ms/拍)
 */
void ProcessKey(volatile Key_t *key)
{
    uint8_t level = GPIO_ReadInputDataBit(key->port, key->pin);

    switch (key->state)
    {
    case KEY_STATE_IDLE: // 空闲:问"按下了吗?"
        if (level == 0)
        {
            key->state = KEY_STATE_DEBOUNCE;
            key->debounce_count = 0;
        }
        break;

    case KEY_STATE_DEBOUNCE: // 按下消抖:连续3次采样=30ms 连续低 → 确认按住
        if (level == 0)
        {
            if (++key->debounce_count >= 3)
            {
                key->state = KEY_STATE_PRESSED;
                key->press_start_time = xTaskGetTickCount();
                key->event_flag = KEY_EVENT_PRESS;
            }
        }
        else
        {
            key->state = KEY_STATE_IDLE; // 判定为抖动,作废
            key->debounce_count = 0;
        }
        break;

    case KEY_STATE_PRESSED: // 已确认按下,检测释放
        if (level == 1)
        {
            key->debounce_count = 0;
            key->state = KEY_STATE_RELEASE;
        }
        break;

    case KEY_STATE_RELEASE: // 释放消抖:连续3次为高才确认释放 → 判定短按/长按
        if (level == 1)
        {
            if (++key->debounce_count >= 3)
            {
                uint32_t duration = xTaskGetTickCount() - key->press_start_time;
                key->event_flag = (duration > LONG_PRESS_THRESHOLD)
                                      ? KEY_EVENT_LONG_PRESS
                                      : KEY_EVENT_SHORT_PRESS;
                key->state = KEY_STATE_IDLE;
            }
        }
        else // 没松干净,回 PRESSED 重新检测
        {
            key->state = KEY_STATE_PRESSED;
            key->debounce_count = 0;
        }
        break;
    }
}

// ==================== 任务层 ====================

/**
 * @brief 生产者:按键扫描任务,每 10ms 扫一次状态机
 * @note vTaskDelayUntil 固定节拍不漂移;有 SHORT/LONG 事件就塞进队列
 */
static void key_scan_task(void *arg)
{
    TickType_t lastWake = xTaskGetTickCount(); // 节拍锚点，用来固定 10ms 一拍(消抖30ms=3拍)
    uint8_t evt;//用来存储按键事件

    for (;;)
    {
        ProcessKey(&keys[1]); // 状态机在任务上下文跑,不进中断

        // 把事件从信箱取走,发往处理任务(PRESS 只用于状态推进,不发给业务层)
        if (keys[1].event_flag == KEY_EVENT_SHORT_PRESS ||
            keys[1].event_flag == KEY_EVENT_LONG_PRESS)
        {
            evt = keys[1].event_flag;
            keys[1].event_flag = KEY_EVENT_NONE;
            xQueueSend(key_queue, &evt, 0); // 队列满就丢:按键事件丢了无损失
        }

        vTaskDelayUntil(&lastWake, 10); // 固定 10ms 一拍(消抖30ms=3拍)
    }
}

/**
 * @brief 消费者:业务处理任务,想加功能只改这里
 * @note 没事件时阻塞在 xQueueReceive,不占 CPU
 */
static void key_action_task(void *arg)
{
    uint8_t evt;//用来接收按键事件

    for (;;)
    {
        if (xQueueReceive(key_queue, &evt, portMAX_DELAY) == pdPASS)//portMAX_DELAY 的效果是：任务不会主动轮询，也不会占用 CPU 等待，而是进入阻塞状态，直到队列有数据。若启用了 FreeRTOS 的时间片配置，通常还会配合队列等待超时使用。
        {
            switch (evt)
            {
            case KEY_EVENT_SHORT_PRESS:
                LED_RED_toggle(); // 短按 → 红灯翻转
                break;
            case KEY_EVENT_LONG_PRESS:
                flag = 1; // 长按 → 置标志 + 蓝灯翻转(看得见的反馈)
                LED_BLUE_toggle();
                break;
            default:
                break;
            }
        }
    }
}

/**
 * @brief 按键模块初始化:建队列 → 配 GPIO → 创建两个任务
 * @note 在 vTaskStartScheduler() 之前调用
 */
void key_tasks_init(void)
{
    key_queue = xQueueCreate(8, sizeof(uint8_t)); // 8 个事件缓冲
    key_Config();
    xTaskCreate(key_scan_task, "key_scan", 256, NULL, 2, NULL);
    xTaskCreate(key_action_task, "key_action", 128, NULL, 3, NULL); // 高一点,响应快
}
