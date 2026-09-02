#include "FreeRTOS.h"
#include "task.h"
#include "main.h"


//pa9 u1_tx  pa10 u1_rx
//cp2102连在串口1上
int fputc(int c, FILE *stream)
{
    while ((USART1->SR & (1 << 7)) == 0)
        ; // 当发送数据缓冲区不为空时，循环发送,直到发送完毕
    USART1->DR = c;
    return c;
}


void vApplicationMallocFailedHook(void)
{
    for (;;);    /* 堆内存分配失败（configTOTAL_HEAP_SIZE 不够） */
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    for (;;);    /* 任务栈溢出（栈配小了或递归太深） */
}

static void led_task(void *arg)
{
    printf("led task run\r\n");                                // ← 加
    while (1)
    {
        LED_RED_toggle();
        vTaskDelay(500);
    }
}


int main()
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    TIM10_Task_Init(84,1000);
    usart1_ini(115200);
    led_ini();
    L9110S_ini();
    spi1_w25_ini();
    key_ini();

    printf("before sched\r\n");                                // ← 加
    BaseType_t rc = xTaskCreate(led_task, "led", 128, NULL, 1, NULL);
    printf("create rc=%d\r\n", (int)rc);                       // ← 加，1=成功
    vTaskStartScheduler();
    printf("sched returned!\r\n");                             // ← 加，打到这=启动失败
    for (;;);
}

