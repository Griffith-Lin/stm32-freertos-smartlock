#include "key.h"

volatile Key_t keys[3] = {0};


/* ── QueueHandle_t ──────────────────────────────────────────────────────
 * 队列的"取货凭证"。拿着它才能 Send / Receive,但别去拆开看里面
 * (它指向 struct QueueDefinition,里面是环形缓冲 + 两条等待链表)。
 * 顺带一提:**它和信号量的 SemaphoreHandle_t 是同一个类型** ——
 * queue.h 里 typedef 成 QueueHandle_t,semphr.h 里又 typedef 成
 * SemaphoreHandle_t。因为信号量/互斥量底下本来就是队列,只是每项 0 字节。
 * 这里初始化成 NULL 是个习惯:调度器启动前它确实是空的,
 * 谁要是提前用了就撞 configASSERT,而不是拿个野指针到处乱跑。
 * 【一句话】队列的句柄 —— 建队列时拿到它,以后收发货都凭它。 */
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
    uint8_t evt;

    for (;;)
    {
        ProcessKey(&keys[1]); // 状态机在任务上下文跑,不进中断

        // 把事件从信箱取走,发往处理任务(PRESS 只用于状态推进,不发给业务层)
        if (keys[1].event_flag == KEY_EVENT_SHORT_PRESS ||
            keys[1].event_flag == KEY_EVENT_LONG_PRESS)
        {
            evt = keys[1].event_flag;
            keys[1].event_flag = KEY_EVENT_NONE;
            /* ── xQueueSend(队列, &要发的数据, 最多等多久) ──────────────────
             * 把数据**拷贝**进队列(内核里就一句 memcpy)。
             * 所以发完这句,evt 这个局部变量随便改都行 —— 队列里存的是副本。
             * 这就是队列和"传指针"最大的区别。
             *
             * 第 3 个参数 = "队满了怎么办":
             *     0                 = 不等,塞不进立刻返回 errQUEUE_FULL(丢掉)
             *     pdMS_TO_TICKS(100)= 最多等 100ms
             *     portMAX_DELAY     = 死等,一直等到有人取走腾出空位
             * 这里**必须**写 0:本任务用 vTaskDelayUntil 卡着 10ms 的节拍,
             * 一旦在这儿阻塞,整个扫描周期就乱了 —— 按键手感直接变差。
             * 而按键事件丢了没损失(用户再按一下就是),所以"丢"是正确选择。
             * 返回值:pdPASS = 放进去了 / errQUEUE_FULL = 满了没放进去。
             *
             * 【一句话】把一条数据"放上传送带" —— 队满时是等着还是丢掉,
             *           由第 3 个参数说了算。 */
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
    uint8_t evt;

    for (;;)
    {
        /* ── xQueueReceive(队列, &收到哪, 最多等多久) ─────────────────────
         * 从队头取走一条(读 + 删),同样是**拷贝**到你给的缓冲区。
         * 想"只看不删"用 xQueuePeek。
         *
         * 第 3 个参数是 portMAX_DELAY = 死等:没事件就一直睡在这儿,
         * 一点 CPU 都不占。有人 xQueueSend 的那一瞬间,内核把它从
         * "等收货"链表搬回就绪链表 —— 这就是"事件驱动",而不是轮询。
         * (顺带:portMAX_DELAY 的任务会被放进挂起链表,但状态仍是**阻塞态**,
         *  因为没有超时能叫醒它 —— 上一课那个坑在这里又出现一次。)
         *
         * 返回值:pdPASS = 取到了 / pdFAIL = 超时(死等时基本不会发生)。
         *
         * 【一句话】站在传送带尽头"接货" —— 有货就拿走接着干活,
         *           没货就睡觉,不白烧 CPU。 */
        if (xQueueReceive(key_queue, &evt, portMAX_DELAY) == pdPASS)
        {
            switch (evt)
            {
            case KEY_EVENT_SHORT_PRESS:
                LED_RED_toggle(); // 短按 → 红灯翻转
                break;
            case KEY_EVENT_LONG_PRESS:
                
                LED_GREEN_toggle();
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
    /* ── xQueueCreate(能装几项, 每项几字节) ─────────────────────────────
     * ① 第一个参数是**项数,不是字节数**。8 = "最多存 8 条事件"。
     * ② 第二个参数是**每条多大**。sizeof(uint8_t) = 1。
     *    两者相乘就是存储区(8 字节),再加上 Queue_t 结构体本身,
     *    一次 pvPortMalloc 全拿走 —— 所以建队列是要吃堆的。
     *    发一条 200 字节的结构体,存储区就是 8×200 = 1600 字节。
     * ③ 数据是**按拷贝入队**的(queue.h 原话:"Items are queued by copy,
     *    not by reference"),所以每项的大小必须固定且一致。
     * ④ 这里是宏,展开就是 xQueueGenericCreate(...) —— 建对象都在调用者
     *    上下文里直接完成,不经过任何队列,所以能在 vTaskStartScheduler()
     *    之前安全调用(和 xTimerCreate 一个道理)。
     * 返回值:句柄;堆不够返回 NULL。NULL 传下去会撞 configASSERT 死循环。
     *
     * 为什么只给 8 个:按键事件是"最新鲜才有用"的东西。用户连按十下,
     * 丢掉最老的、保住最新的反而手感更好;队列再大也只是把处理延迟
     * 攒得更长,不是更好。
     *
     * 【一句话】开一条"最多存 N 条、每条 M 字节"的传送带 —— 建完是空的,
     *           之后谁都能往里放。 */
    key_queue = xQueueCreate(8, sizeof(uint8_t)); // 8 个事件缓冲
    key_Config();
    xTaskCreate(key_scan_task, "key_scan", 256, NULL, 2, NULL);
    xTaskCreate(key_action_task, "key_action", 128, NULL, 3, NULL); // 高一点,响应快
}
