#include "main.h"
#include "FreeRTOS.h"
#include "learn.h"    // 学习演示任务(learn.c):互斥量/计数信号量/LCD测试等,与业务无关
#include "mcu_wake.h" // PA0 待机唤醒(业务)
#include "task.h"

// pa9 u1_tx  pa10 u1_rx

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

void vApplicationIdleHook(void) // FreeRTOS 空闲任务钩子函数,由 configUSE_IDLE_HOOK 使能
{
    // LED_BLUE_toggle();空闲状态可视化
}

/**
 * @brief 主函数:外设初始化后创建任务并启动 FreeRTOS 调度器
 * @return 无
 * @note 正常情况下 vTaskStartScheduler 不会返回
 */
int main()
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    mcu_wkup_Config(); // 开 PWR 时钟 + 配 PA0(WKUP),须最先于其他外设初始化

    // ===== 外设初始化 =====
    TIM10_Task_Config(84, 1000); // 1ms 毫秒时基(TIM10 中断维护 tim10_tick_ms)
    usart1_Config(115200);
    led_Config();
    L9110S_Config();
    spi1_w25_Config();
    LCD_Init();
    mg200_Config(115200);
    i2c_master_Config();
    CY9CM_Init(); // RES 复位脉冲 + (必要时)写 128 字节配置


    // ===== 待机唤醒识别(复位原因分流)=====
    // STANDBY 唤醒 = 整片复位,一切归零重跑;靠 WUF 标志认亲,分流出不同启动流程
    if (mcu_was_standby_wake()) // 长按 PC13 进待机 → 按 PA0 唤醒后走这里
    {
        printf("wake from STANDBY!\r\n"); // 冷启动不会打印这一行
        LED_BLUE_toggle();                // 亮绿灯标记"我从待机醒来"(冷启动保持灭)
    }

    // ===== 业务任务 =====
    key_tasks_init(); // 按键:建事件队列 + 创建 key_scan(生产)/ key_action(消费) 两个任务
    cy9cm_tasks_init();

    // ===== 学习演示任务(与业务无关,平时可整组注释) =====
    // learn_tasks_init();

    vTaskStartScheduler(); // 启动调度器,正常情况下不返回

    printf("sched returned!\r\n"); // ← 打到这=启动失败
    for (;;)
        ;
}
