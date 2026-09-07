#include "main.h"
#include "FreeRTOS.h"
#include "task.h"

// pa9 u1_tx  pa10 u1_rx
// cp2102连在串口1上
int fputc(int c, FILE *stream)
{
    while ((USART1->SR & (1 << 7)) == 0)
        ; // 当发送数据缓冲区不为空时，循环发送,直到发送完毕
    USART1->DR = c;
    return c;
}

void vApplicationMallocFailedHook(void)//这两个 Hook 是 FreeRTOS 的错误处理机制：发生致命错误时，如果直接返回，任务调度还会继续用坏掉的堆/栈运行，导致更隐蔽的问题，所以惯例就是就地死循环"停住"，并配合 printf/断点排查。
{
    for (;;)//卡死在这，区分于while(1)——后者是任务主循环，调用 vTaskDelay(500) 主动让出 CPU，不是卡死，而是 FreeRTOS 任务的标准写法。
        ; /* 堆内存分配失败（configTOTAL_HEAP_SIZE 不够） */
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    for (;;)
        ; /* 任务栈溢出（栈配小了或递归太深） */
}

static void led_task(void *arg)
{

    while (1)//while(1) 则是有意的任务主循环——调用 vTaskDelay(500) 主动让出 CPU，不是卡死，而是 FreeRTOS 任务的标准写法。
    {
        LED_RED_toggle();
        vTaskDelay(500);
    }
}

static void print_task(void *arg)
{

    while (1)
    {
        printf("111\r\n");
        vTaskDelay(500);
    }
}

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
int main()
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    TIM10_Task_Config(84, 1000);
    usart1_Config(115200);
    led_Config();
    L9110S_Config();
    spi1_w25_Config();
    key_Config();
    LCD_Init();


    /*
    BaseType_t xTaskCreate(
    TaskFunction_t      pxTaskCode,    // ① 任务函数指针
    const char * const  pcName,        // ② 任务名字（仅调试用）
    configSTACK_DEPTH_TYPE usStackDepth, // ③ 栈深度（单位：字！）
    void *              pvParameters,  // ④ 传给任务函数的参数
    UBaseType_t         uxPriority,    // ⑤ 优先级
    TaskHandle_t *      pxCreatedTask  // ⑥ 任务句柄（输出参数）
    );

    */
    BaseType_t rc1 = xTaskCreate(led_task, "led", 128, NULL, 3, NULL);

    BaseType_t rc2 = xTaskCreate(print_task, "print_task", 128, NULL, 1, NULL);

    BaseType_t rc3 = xTaskCreate(lcd_task, "lcd_task", 128, NULL, 2, NULL);

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
