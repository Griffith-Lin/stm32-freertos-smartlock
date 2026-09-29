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
#define INCLUDE_vTaskDelay            1   /* 启用 vTaskDelay() 延时 API */
#define INCLUDE_vTaskDelete           1   /* 启用 vTaskDelete() 删除任务 API */
#define INCLUDE_uxTaskPriorityGet     1   /* 启用 uxTaskPriorityGet() 查询任务优先级 API */
#define INCLUDE_xTaskDelayUntil       1   /* 启用 xTaskDelayUntil() 固定周期延时 API */
#define INCLUDE_xTaskGetSchedulerState 1  /* 启用 xTaskGetSchedulerState()：查询"调度器跑起来没有" */
    /* 为什么 LCD 互斥量非开这个不可：lcd.c 的画屏函数在**调度器启动之前**就要用
       （main 里的 LCD_Init、开机画面、MG200 录入提示，全都还在裸机阶段）。
       xSemaphoreTake 一旦走进"拿不到锁就阻塞"的分支，会去操作 pxCurrentTCB，
       而那时内核连第一个任务都还没创建，pxCurrentTCB 是 NULL → 直接 HardFault。
       所以取锁前必须先问一句"调度器在跑吗"，没跑就跳过锁——裸机阶段本来就只有
       主线程在碰 LCD，不存在竞争，跳过是安全的。
       ⚠ 只判断互斥量句柄是否为 NULL 是不够的：LCD_Init 里就把锁建好了，句柄非空，
         但那时离 vTaskStartScheduler() 还差着十万八千里。 */

//以后凡是链接报"Undefined symbol"而头文件里明明有声明，先怀疑是不是没开对应的 INCLUDE_xxx 开关——这是 FreeRTOS 为了省 flash/RAM 故意做的裁剪，编译器抓不到，只有链接器能抓到。

#define configUSE_PREEMPTION            1   /* 启用抢占式调度：高优先级就绪任务可抢占低优先级任务 */
#define configMAX_PRIORITIES            (5)      /* 支持的任务优先级数量；实际优先级为 0~4 */
#define configMINIMAL_STACK_SIZE        (128)    /* 空闲任务最小栈深度，单位为字；32 位 MCU 中 128 字=512 字节 */
// 从 8KB 提到 12KB:加 wifi 任务(栈 384 字 = 1536 字节 + TCB)之前实测只剩 1240 字节,
// 8KB 已经装不下了。F401 一共 96KB RAM,当时只用掉 17.7KB,提这 4KB 毫无压力 ——
// 堆大小纯粹是配置选择,不是硬件限制。以后再往里加任务,先读 xFreeBytesRemaining 看余量——
// ⚠ 别记地址:它每次编译都可能变(加一个全局变量就漂)。用这条命令现查:
//     arm-none-eabi-nm.exe "build/Target 1/project.axf" | grep xFreeBytesRemaining
#define configTOTAL_HEAP_SIZE           (12 * 1024) /* FreeRTOS 动态内存堆大小，单位为字节 */
#define configMAX_TASK_NAME_LEN         (12)     /* 任务名称缓冲区最大长度，通常包含结尾的 '\0' */
#define configIDLE_SHOULD_YIELD         1        /* 同优先级任务就绪时，允许空闲任务主动让出 CPU */
#define configUSE_MUTEXES               1        /* 启用互斥量及其优先级继承机制 */
#define configUSE_RECURSIVE_MUTEXES     1        /* 启用递归互斥量 */
#define configUSE_COUNTING_SEMAPHORES   1        /* 启用计数信号量 */
#define configQUEUE_REGISTRY_SIZE       8        /* 队列/信号量注册表容量，用于调试查看对象名称 */

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
