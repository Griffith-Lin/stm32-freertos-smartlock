# STM32F401 FreeRTOS 移植全流程实录

环境：STM32F401RETx · FreeRTOS-Kernel V11.3.0（LTS）· Keil AC5（eIDE 工程）· 84MHz
源码位置：D:\迅雷下载\FreeRTOS-LTS\FreeRTOS\FreeRTOS-Kernel
移植目标：LED 闪烁任务跑通，为后续把按键/电机/串口模块改成任务铺路

## 第 0 步：移植前的分析（决定后面所有选择的三个事实）

| 事实 | 结论 |
|---|---|
| 工具链是 AC5（armcc5） | 移植层必须选 portable/RVDS/ARM_CM4F/ ，不是 GCC |
| 芯片是 Cortex-M4F | 需要带 FPU 的 ARM_CM4F 移植层 |
| 启动文件 SVC/PendSV/SysTick 是弱定义 | 用宏映射接管，不用改启动文件 |

## 第 1 步：复制内核源码

目标目录（g:\github\train2\freertos\ ）：

```
freertos\
├── src\       ← tasks.c queue.c list.c timers.c event_groups.c stream_buffer.c
├── include\   ← 内核整个 include 目录（FreeRTOS.h task.h queue.h ...）
├── port\      ← portable\RVDS\ARM_CM4F\ 下的 port.c + portmacro.h
├── heap\      ← portable\MemMang\heap_4.c
└── FreeRTOSConfig.h  ← 自己创建（第 2 步）
```

复制命令（Git Bash）：

```
K="/d/迅雷下载/FreeRTOS-LTS/FreeRTOS/FreeRTOS-Kernel"
mkdir -p /g/github/train2/freertos/{src,include,port,heap}
cp $K/tasks.c $K/queue.c $K/list.c $K/timers.c $K/event_groups.c $K/stream_buffer.c /g/github/train2/freertos/src/
cp $K/include/*.h /g/github/train2/freertos/include/
cp $K/portable/RVDS/ARM_CM4F/port.c $K/portable/RVDS/ARM_CM4F/portmacro.h /g/github/train2/freertos/port/
cp $K/portable/MemMang/heap_4.c /g/github/train2/freertos/heap/
```

不复制 croutine.c（协作式任务，用不到）。堆管理选 heap_4（支持 free + 碎片合并，最常用）。

## 第 2 步：FreeRTOSConfig.h 最终版（每个坑都在这）

```
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*---------------- 时间与节拍 ----------------*/
#define configCPU_CLOCK_HZ              (84000000UL)   /* F401 主频 */
#define configTICK_RATE_HZ              (1000)         /* 1ms 节拍，与 TIM10 一致 */
#define configTICK_TYPE_WIDTH_IN_BITS   TICK_TYPE_WIDTH_32_BITS
    /* V11 强制要求，替代旧版 configUSE_16_BIT_TICKS（两者同定义反而报错） */

/*---------------- 任务与内存 ----------------*/
#define INCLUDE_vTaskDelay              1        /* 坑：不定义则 vTaskDelay 不编译，链接报 undefined */
#define configUSE_PREEMPTION            1        /* 坑：无默认值，缺失直接 #error */
#define configMAX_PRIORITIES            (5)
#define configMINIMAL_STACK_SIZE        (128)    /* 单位是"字"(4字节) */
#define configTOTAL_HEAP_SIZE           (8 * 1024)
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
#define configUSE_IDLE_HOOK             0
#define configUSE_TICK_HOOK             0
#define configUSE_MALLOC_FAILED_HOOK    1    /* 开了必须实现 vApplicationMallocFailedHook */
#define configCHECK_FOR_STACK_OVERFLOW  2    /* 开了必须实现 vApplicationStackOverflowHook */

#define configASSERT(x)  if ((x) == 0) { taskDISABLE_INTERRUPTS(); for (;;); }

/*---------------- 中断优先级 ----------------*/
#define configPRIO_BITS                 4
#define configKERNEL_INTERRUPT_PRIORITY         (255)
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    ( 0xB0 )
    /* 坑：F401 只有 4 位优先级，此值必须是 16 的倍数。
       191(0xBF) 的低 4 位非零，xPortStartScheduler 里的断言死循环 */

/*---------------- 启动文件符号映射（V11 命名！）----------------*/
#define vPortSVCHandler       SVC_Handler
#define vPortPendSVHandler    PendSV_Handler     /* 旧版名字，V11 已不用 */
#define vPortSysTickHandler   SysTick_Handler    /* 旧版名字，V11 已不用 */
#define xPortPendSVHandler    PendSV_Handler     /* 坑：V11 把 PendSV/SysTick 改名为 xPort* */
#define xPortSysTickHandler   SysTick_Handler

#endif
```

## 第 3 步：eIDE 工程集成

- 8 个 .c 全部加入工程（src 6 + port.c + heap_4.c）
- 头文件路径加三个：freertos 、freertos/include 、freertos/port
- 手工改的话编辑 .eide/eide.yml ：虚拟文件夹加 freertos 组，incList 加三个路径

## 第 4 步：main.c 改造

```c
#include "FreeRTOS.h"
#include "task.h"

void vApplicationMallocFailedHook(void)      /* 配置里开了钩子开关就必须实现 */
{
    for (;;);
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    for (;;);
}

static void led_task(void *arg)
{
    while (1)
    {
        LED_RED_toggle();
        vTaskDelay(500);      /* 任务延时：让出 CPU，而非忙等 */
    }
}

int main()
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);   /* 必须 Group_4，4 位全抢占 */
    /* ...原有外设初始化不变... */
    xTaskCreate(led_task, "led", 128, NULL, 1, NULL);
    vTaskStartScheduler();    /* 永不返回 */
    for (;;);
}
```

## 第 5 步：踩坑全记录（按排查顺序，共 6 个）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | #error Missing definition: configUSE_PREEMPTION | 模板漏了该配置（无默认值） | 补 #define configUSE_PREEMPTION 1 |
| 2 | L6200E: Symbol SVC_Handler multiply defined (stm32f4xx_it.o and port.o) | 标准库模板 it.c 里三个空 handler 是强定义，与移植层冲突 | 注释掉 it.c 的 SVC/PendSV/SysTick 三个空函数 |
| 3 | L6218E: vApplicationMallocFailedHook / vApplicationStackOverflowHook / vTaskDelay undefined | 前两个：开关开了但 hook 函数没实现；vTaskDelay：INCLUDE_vTaskDelay 没开，函数被编译期裁剪 | main.c 里实现两个 hook；配置里开 INCLUDE_vTaskDelay |
| 4 | 编译链接全过，串口卡在 create rc=1 ，无任何输出 | configMAX_SYSCALL_INTERRUPT_PRIORITY=191(0xBF) 低 4 位非零，port.c:365 断言死循环（F401 优先级只实现 4 位 0xF0） | 改成 0xB0 （176），16 的倍数 |
| 5 | 同上现象，全量重编后依旧 | 见 #6 的真正根因 | — |
| 6 | 同上现象（最终根因） | V11 把 PendSV/SysTick 处理器改名为 x P o r t P e n d S V H a n d l e r / x P o r t S y s T i c k H a n d l e r ，旧宏映射对不上；加上函数级裁剪（one-elf-section-per-function ），链接器静默丢弃移植层、向量表回退到启动文件的 B . 空壳 → 第一个 SysTick 中断死循环 | 补 xPort* 两个宏映射 |

## 第 6 步：定位问题的方法（以后复用）

1. 串口探针二分法：在 main 的每个关键节点打印（before sched / create rc / sched returned / led task run ），一次编译就能把死点压缩到一个函数内
2. map 文件照妖镜：build/Target 1/project.map 里查符号定义归属：

```
grep "PendSV_Handler\|SysTick_Handler" "build/Target 1/project.map"
```

   定义在 port.o → 移植层接管 ✓
   定义在 startup_stm32f401xx.o(.text) 且只有 2 字节 → 弱空壳，映射失败 ✗

3. 调试器看 PC：PC 停在哪决定问题类型（configASSERT 循环 / HardFault_Handler / Default_Handler / 任务代码）

## 第 7 步：移植完成后的铁律（模块改造前必读）

1. 任务里禁止用 TIM10_delay 忙等 → 一律换 vTaskDelay()
2. 中断优先级规则：优先级数值 ≥ 11（0xB0 语义）的中断才能调 FromISR API。TIM10（优先级 1）和 EXTI（3）都不能调——按键事件要放到任务里消费
3. ISR 职责：清标志 + 置事件/发信号量，业务逻辑放任务
4. 共享变量：跨任务/跨 ISR 的变量要么用队列/互斥量，要么临界区
5. 新用 API 前查开关：INCLUDE_vTaskDelete 、INCLUDE_xTaskDelayUntil 等默认关闭，用了先开
6. 改头文件后必须全量重编：armcc5 不总是跟踪头文件依赖，只改配置不重编 = 烧旧固件（坑 #4 的教训）


