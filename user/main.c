#include "main.h"

#include "FreeRTOS.h"//freertos的核心头文件。  必须第一个包含，其他 FreeRTOS 头文件（task.h、semphr.h 等）都依赖它。它基于 FreeRTOSConfig.h 提供 BaseType_t、TickType_t、pdTRUE/pdMS_TO_TICKS() 这些基础类型和宏。
#include "task.h"//任务。  xTaskCreate() 创建任务、vTaskDelay() 延时、vTaskStartScheduler() 启动调度器、vTaskDelete() 等。
#include "semphr.h"//互斥锁。  xSemaphoreCreateMutex()、xSemaphoreTake()、xSemaphoreGive()、xSemaphoreCreateBinary() 等。因为 FreeRTOS 的信号量/互斥量底层是基于队列实现的，所以它内部会 #include "queue.h"——你包含了 semphr.h 就等于也拿到了队列 API（xQueueCreate、xQueueSend、xQueueReceive）。

SemaphoreHandle_t  print_mutex;

/* ==================== 第 5 课:互斥量 ====================
 * 串口是共享资源。printf 经 fputc 逐字符写 USART1->DR,一旦被高优先级任务抢断,
 * 会造成两种破坏:
 *   ① 穿插:两行字符互相切碎
 *   ② 丢字符(更隐蔽):task A 查完 TXE 被抢走,task B 写完 DR,A 回来又写一次
 *      → B 那个字节被覆盖丢失(USART 的 DR 一次只存 1 个待发字节)
 * 所以锁的粒度必须等于"一整行"这个原子单位,锁要包住整个 printf。
 * 加在 fputc 里挡不住 —— 那样每个字符都是一段合法临界区,B 照样能插进 A 的行中间。
 */
#define PRINT_LOCK_ENABLED 1 // 实验开关:0=无锁对照组 1=加锁。两轮之间只改这一个值,其余全不动

/**
 * @brief 整行原子打印(第 5 课的观测点)
 * @param s 要打印的字符串(不含换行,本函数自己补 \r\n)
 * @note 无锁时被优先级 4 的 print_task2 拦腰打断 → 串口出现混行/少字符（观测：不加锁确实出现了混行和少字符的现象）
 *       加锁后 xSemaphoreTake 拿不到就进 Blocked,不占 CPU,直到持有者 Give
 */
static void print_line(const char *s)
{
#if PRINT_LOCK_ENABLED
    xSemaphoreTake(print_mutex, portMAX_DELAY); // 拿不到就睡,不用轮询空耗
    printf("%s\r\n", s);
    xSemaphoreGive(print_mutex);                // 必须放!漏掉 = 其它任务拿不到锁，因此永久阻塞在 Take
#else
    printf("%s\r\n", s);
#endif
}


// pa9 u1_tx  pa10 u1_rx
// cp2102连在串口1上
int fputc(int c, FILE *stream)
{
    while ((USART1->SR & (1 << 7)) == 0)
        ; // 当发送数据缓冲区不为空时，循环发送,直到发送完毕
    USART1->DR = c;
    return c;
}

// 这两个 Hook 是 FreeRTOS 的错误处理机制：发生致命错误时，如果直接返回，任务调度还会继续用坏掉的堆/栈运行，导致更隐蔽的问题，所以惯例就是就地死循环"停住"，并配合 printf/断点排查。
/**
 * @brief FreeRTOS 堆内存分配失败回调(由 configUSE_MALLOC_FAILED_HOOK 使能)
 * @return 无
 * @note 分配失败多为 configTOTAL_HEAP_SIZE 过小;卡死便于断点排查
 */
void vApplicationMallocFailedHook(void)
{
    for (;;) // 卡死在这，区分于while(1)——后者是任务主循环，调用 vTaskDelay(500) 主动让出 CPU，不是卡死，而是 FreeRTOS 任务的标准写法。
        ;    /* 堆内存分配失败（configTOTAL_HEAP_SIZE 不够） */
}

/*
@brief 任务栈溢出钩子函数
FreeRTOS 任务栈溢出时会调用这个函数，参数：

TaskHandle_t xTask —— 出事任务的"身份证指针"，指向内核的 TCB（任务控制块）,xTaskCreate 最后一个参数 pxCreatedTask 让你拿回这个指针；你当时传了 NULL 表示"我不要"，但 FreeRTOS 内部自己一直存着
任务出事后，FreeRTOS 把出事任务自己的户口本地址塞给 Hook 的第一个参数，这样你就能知道是哪个任务出事了

char *pcTaskName —— 出事任务的"名字"

FreeRTOS 要求 Hook 里要么 for(;;) 停住，要么删掉肇事任务——因为它的栈已经不可信了，让它继续跑只会更乱。
 */
/**
 * @brief FreeRTOS 任务栈溢出回调(由 configCHECK_FOR_STACK_OVERFLOW 使能)
 * @param xTask 溢出任务的句柄(TCB 指针)
 * @param pcTaskName 溢出任务的名字
 * @return 无
 * @note 任务栈已不可信,打印现场后删除肇事任务并停住
 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    printf("stack overflow %s\r\n", pcTaskName);
    // printf("优先级 = %d\r\n", uxTaskPriorityGet(xTask));  // 查它的优先级
    vTaskDelete(xTask); // 或者：把肇事任务直接"枪毙"，让别的任务继续跑
    for (;;)
        ; /* 任务栈溢出（栈配小了或递归太深） */
}


void vApplicationIdleHook(void) // FreeRTOS 空闲任务钩子函数,由 configUSE_IDLE_HOOK 使能。如果有一个任务不再睡觉，那空闲函数就会饿死
{
    // LED_BLUE_toggle();           // 根据led的状态来看进空闲任务的频率
    print_line("6666666666666666666/r/n");
}


/*
我没给那两个 Hook 里的 printf 加锁：Hook 触发时系统已经不可信，如果某个任务正握着锁，Hook 再去 Take 就是自己等自己的死锁


*/




/*-----------------任务----------------------*/

static void led_task(void *arg)
{

    while (1) // while(1) 则是有意的任务主循环——调用 vTaskDelay(500) 主动让出 CPU，不是卡死，而是 FreeRTOS 任务的标准写法。
    {
        // die(5); 模拟栈溢出
        vTaskDelay(500);
    }
}

/**
 * @brief 低优先级"长工":死循环狂打长行,给高优先级任务制造拦腰打断的机会
 * @param arg 任务参数(未使用)
 * @return 无(永不返回)
 * @note 48 字符 @115200 8N1 ≈ 4.2ms,必然跨过好几个 1ms tick 边界
 *       故意去掉 vTaskDelay:它一睡觉 CPU 就空了,print_task2 醒来时根本没人在打印,撞不上
 *       副作用:idle 任务(优先级 0)从此被饿死 —— 正好是"低优先级霸占 CPU 会饿死 idle"的活标本
 */
static void print_task1(void *arg)
{
    while (1)
    {
        print_line("111111111111111111111111111111111111111111111111");
    }
}

/**
 * @brief 高优先级"插话者":每 100ms 醒来打断 print_task1 一次
 * @param arg 任务参数(未使用)
 * @return 无(永不返回)
 * @note 优先级 4 > 2,醒来的瞬间立刻抢占(不等下一个 tick),正好砍在 task1 的行中间
 *       周期从 1000ms 缩到 100ms,让穿插现象每秒稳定出现约 10 次,便于观察
 */
static void print_task2(void *arg)
{
    TickType_t lastWakeTime = xTaskGetTickCount(); // 锚点：进循环前对准一次

    while (1)
    {
        print_line("222222222222222222222222222222222222222222222222");
        vTaskDelayUntil(&lastWakeTime, 100); // 固定在"上次 + 100ms"醒，不漂移
    }
}

/**
 * @brief LCD 刷屏测试任务
 * @param arg 任务参数(未使用)
 * @return 无
 * @note 每 500ms 白/黑全屏交替,用于验证 LCD 驱动
 */
static void lcd_task(void *arg)
{
    while (1)
    {
        LCD_Clear(0, 240, 0, 320, WHITE);
        vTaskDelay(500);
        LCD_Clear(0, 240, 0, 320, BLACK);
        vTaskDelay(500);
    }
}



/**
 * @brief 主函数:外设初始化后创建任务并启动 FreeRTOS 调度器
 * @return 无
 * @note 正常情况下 vTaskStartScheduler 不会返回
 */
int main()
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    TIM10_Task_Config(84, 1000);
    usart1_Config(115200);
    led_Config();
    L9110S_Config();
    spi1_w25_Config();
    LCD_Init();
    key_tasks_init(); // 建队列 + 创建 key_scan(生产事件)/ key_action(消费) 两个任务

    /*BaseType_t xTaskCreate(
    TaskFunction_t      pxTaskCode,    // ① 任务函数指针
    const char * const  pcName,        // ② 任务名字（仅调试用）
    configSTACK_DEPTH_TYPE usStackDepth, // ③ 栈深度（单位：字！）32位系统：1字 = 4字节（32位）
    void *              pvParameters,  // ④ 传给任务函数的参数
    UBaseType_t         uxPriority,    // ⑤ 优先级，区分于stm32的NVIC优先级，FreeRTOS优先级数值越大，任务优先级越高
    TaskHandle_t *      pxCreatedTask  // ⑥ 任务句柄（输出参数）
    );

    */
    BaseType_t rc1 = xTaskCreate(led_task, "led_task", 128, NULL, 3, NULL);

    BaseType_t rc2 = xTaskCreate(print_task1, "print_task1", 128, NULL, 2, NULL);

    BaseType_t rc3 = xTaskCreate(print_task2, "print_task2", 128, NULL, 4, NULL); // 优先级最高，防止在打印的时候，被别的任务抢走 CPU，导致打印的时间戳不准确

    BaseType_t rc4 = xTaskCreate(lcd_task, "lcd_task", 128, NULL, 2, NULL);

    /* 第 5 课:互斥量必须在 vTaskStartScheduler() 之前创建 —— 它要从 FreeRTOS 堆(heap_4)分配,
     * 调度器一跑起来,任务可能立刻就用它,来不及了。顺带量一下这把锁花了多少字节。(观测：大概100个字节) */
    printf("heap before mutex: %u\r\n", (unsigned)xPortGetFreeHeapSize());
    print_mutex = xSemaphoreCreateMutex();
    printf("heap after  mutex: %u\r\n", (unsigned)xPortGetFreeHeapSize());
    if (print_mutex == NULL)
        printf("mutex create FAILED: heap not enough\r\n"); // 创建失败还去 Take,configASSERT 会直接停住

    vTaskStartScheduler();
    /*从堆（heap_4）分配两块内存：
    TCB（任务控制块，内核用 struct tskTaskControlBlock 管理这个任务）+ 任务栈（你说的 128 字）
    调用移植层的 pxPortInitialiseStack 把任务栈预铺成"刚被中断打断的样子"——里面预置了 xPSR、PC（指向你的任务函数）、LR（指向 prvTaskExitError）、R0（参数）——这样第一次"切进去"就像恢复一次中断上下文
    把 TCB 链进就绪链表中优先级 1 的队列
    返回
     */

    printf("sched returned!\r\n"); // ← 加，打到这=启动失败
    for (;;)
        ;
}

/*栈 = 任务干活时用的草稿纸。


函数每次调用别的函数（比如 LED_RED_toggle()）、存临时变量，都要在纸上写几笔。纸不够用，写到"本子外头"去了，就叫栈溢出。

FreeRTOS 怎么查溢出
它有个土办法，像煤窑里的金丝雀：

发给你一叠纸，先用记号笔在最底下那张上画个记号 0xa5
每 500ms 任务切换（任务睡觉换人）时，翻开来看看记号那张纸有没有被写脏
写脏了 = 你的字迹已经写到底了 = 溢出 → 喊你写的那个 Hook 打印
你踩的坑：栈设成 1 个字 = 只发 1 张纸
但任务"出生"时，FreeRTOS 要假装任务是被中断打断的，得先压 8 个数进栈（就像开机要先铺 8 张底稿）。

问题来了：只有 1 张纸，这 8 个数根本放不进你发的这叠纸里，全写到纸堆下面去了——写的是"桌子"。
*/
