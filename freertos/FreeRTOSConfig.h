#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include "stm32f4xx.h"   /* 用里面的 assert_param 之类？不需要，但 F401 的 CMSIS 定义靠它 */

/*---------------- 时间与节拍 ----------------*/
#define configCPU_CLOCK_HZ              (84000000UL)
    /* F401 主频 84MHz。若 SystemCoreClock 不是 84M 会不准 */
#define configTICK_RATE_HZ              (1000)
    /* 1ms 节拍，和你 TIM10 的节拍一致，心理模型无缝衔接 */
#define configTICK_TYPE_WIDTH_IN_BITS   TICK_TYPE_WIDTH_32_BITS
    /* V11 强制要求（替代旧版 configUSE_16_BIT_TICKS），32 位节拍计数器 */

/*---------------- 任务与内存 ----------------*/
#define INCLUDE_vTaskDelay            1
#define INCLUDE_vTaskDelete           1   /* vTaskDelete: 溢出 hook 里"枪毙"任务 */
#define INCLUDE_uxTaskPriorityGet     1   /* hook 里打印任务优先级 */
#define INCLUDE_xTaskDelayUntil       1   /* print_task2 精确周期延时 */
//以后凡是链接报"Undefined symbol"而头文件里明明有声明，先怀疑是不是没开对应的 INCLUDE_xxx 开关——这是 FreeRTOS 为了省 flash/RAM 故意做的裁剪，编译器抓不到，只有链接器能抓到。


#define INCLUDE_vTaskDelay            1
#define configUSE_PREEMPTION            1
#define configMAX_PRIORITIES            (5)      /* 任务优先级 0~4，够用 */
#define configMINIMAL_STACK_SIZE        (128)    /* 单位是"字"(4字节)，128字=512字节 */
#define configTOTAL_HEAP_SIZE           (8 * 1024)  /* 8KB 堆，先这样，不够再加 */
#define configMAX_TASK_NAME_LEN         (12)
#define configIDLE_SHOULD_YIELD         1
#define configUSE_MUTEXES               1
#define configUSE_RECURSIVE_MUTEXES     1
#define configUSE_COUNTING_SEMAPHORES   1
#define configQUEUE_REGISTRY_SIZE       8

/*---------------- 软件定时器 ----------------*/
#define configUSE_TIMERS                1
#define configTIMER_TASK_PRIORITY       (2)
#define configTIMER_QUEUE_LENGTH        (10)
#define configTIMER_TASK_STACK_DEPTH    (128)

/*---------------- 钩子与调试 ----------------*/
#define configUSE_IDLE_HOOK             1  /* 空闲任务钩子 */
#define configUSE_TICK_HOOK             0
#define configUSE_MALLOC_FAILED_HOOK    1
#define configCHECK_FOR_STACK_OVERFLOW  2   /* 检测任务栈溢出，开发期打开 */

#define configASSERT(x)  if ((x) == 0) { taskDISABLE_INTERRUPTS(); for (;;); }
    /* 断言失败 → 关中断死循环，调试时一看 PC 就知道哪里出问题 */

/*---------------- 中断优先级（Cortex-M4 专用）----------------*/
#define configPRIO_BITS                 4
    /* F4 优先级寄存器 4 位，必须配合 NVIC_PriorityGroup_4（第 4 步改） */
#define configKERNEL_INTERRUPT_PRIORITY         (255)
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    ( 0xB0 )   /* 0xB0 = 176，优先级 11，低 4 位必须为 0 */

    /* 0xB0 = 176 = 优先级 11。F401 只实现 4 位优先级（0xF0 掩码），
       此值必须是 16 的倍数，否则 xPortStartScheduler 里的断言会死循环。
       含义不变：优先级数值 ≥ 11（11~15）的中断才能调 fromISR API */


/*---------------- 启动文件符号映射（关键！）----------------*/
#define vPortSVCHandler       SVC_Handler
#define vPortPendSVHandler    PendSV_Handler     /* 旧版名字，V11 已不用 */
#define vPortSysTickHandler   SysTick_Handler    /* 旧版名字，V11 已不用 */
#define xPortPendSVHandler    PendSV_Handler     /* V11 新名字 */
#define xPortSysTickHandler   SysTick_Handler    /* V11 新名字 */

    /* FreeRTOS 端口代码里硬编码用 vPortXXXHandler 这三个名字，
       宏替换成你启动文件里已有的弱符号名（startup_stm32f401xx.s:211-225），
       编译出来就直接覆盖弱定义 → 启动文件一行都不用改 */

#endif
