#include "main.h"
#include "FreeRTOS.h"
#include "task.h"

// pa9 u1_tx  pa10 u1_rx
// cp2102连在串口1上
/**
 * @brief printf 底层发送函数,将单个字符通过 USART1 发出
 * @param c 待发送的字符
 * @param stream 文件流指针(printf 内部使用)
 * @return 发送的字符 c
 */
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

static void led_task(void *arg)
{

    while (1) // while(1) 则是有意的任务主循环——调用 vTaskDelay(500) 主动让出 CPU，不是卡死，而是 FreeRTOS 任务的标准写法。
    {
        // die(5); 模拟栈溢出
        vTaskDelay(500);
    }
}

static void print_task1(void *arg)
{

    while (1)
    {
        // printf("111\r\n");

        // uint8_t level1 = GPIO_ReadInputDataBit(GPIOB, GPIO_Pin_8);
        // printf("level1 = %d\r\n", level);
        // uint8_t level2 = GPIO_ReadOutputDataBit(GPIOB,GPIO_Pin_8);
        // printf("level2 = %d\r\n", level2);

        // uint8_t level3 = GPIO_ReadInputDataBit(GPIOB, GPIO_Pin_9);
        // printf("level3 = %d\r\n", level3);
        // uint8_t level4 = GPIO_ReadOutputDataBit(GPIOB,GPIO_Pin_9);
        // printf("level4 = %d\r\n", level4);

        // printf("key2_in = %d\r\n", GPIO_ReadInputDataBit(GPIOC, GPIO_Pin_13));
        // printf("key2_out = %d\r\n", GPIO_ReadOutputDataBit(GPIOC, GPIO_Pin_13));
        vTaskDelay(500);
    }
}

// 做"每 500ms 精确一次"的闪烁，对比?vTaskDelay?的漂移
static void print_task2(void *arg)
{
    TickType_t lastWakeTime = xTaskGetTickCount(); // 锚点：进循环前对准一次

    while (1)
    {
        // printf("222\r\n");
        vTaskDelayUntil(&lastWakeTime, 1000); // 固定在"上次 + 500ms"醒，不漂移
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

void vApplicationIdleHook(void) // FreeRTOS 空闲任务钩子函数,由 configUSE_IDLE_HOOK 使能
{
    // LED_GREEN_toggle();           // 别忘了分号
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
    key_Config();

    /*
    BaseType_t xTaskCreate(
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

    vTaskStartScheduler();
    /*
     从堆（heap_4）分配两块内存：TCB（任务控制块，内核用 struct tskTaskControlBlock 管理这个任务）+ 任务栈（你说的 128 字）
    调用移植层的 pxPortInitialiseStack 把任务栈预铺成"刚被中断打断的样子"——里面预置了 xPSR、PC（指向你的任务函数）、LR（指向 prvTaskExitError）、R0（参数）——这样第一次"切进去"就像恢复一次中断上下文
    把 TCB 链进就绪链表中优先级 1 的队列
    返回
     */

    printf("sched returned!\r\n"); // ← 加，打到这=启动失败
    for (;;)
        ;
}

/*
栈 = 任务干活时用的草稿纸。

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
