// ==================== 学习演示代码(与业务无关) ====================
// 全部学习用任务集中在这里,main.c 只负责启动编排。
// 从 main.c 搬迁时保持优先级、栈大小、周期一一对应,行为不变。

#include "learn.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

// ==================== 串行打印 ====================
// 多个任务同时 printf 会逐字符交错。print_mutex 保证"一整行"原子输出
static SemaphoreHandle_t print_mutex; // 打印互斥锁(xSemaphoreCreateMutex 带优先级继承)

static void print_line(const char *s)
{
    xSemaphoreTake(print_mutex, portMAX_DELAY); // 获取打印互斥锁；锁被占用时任务进入阻塞态，直到锁可用。第二个参数是最大阻塞时间，参数portMAX_DELAY 通常表示无限等待，但严格来说取决于 FreeRTOSConfig.h 中的 INCLUDE_vTaskSuspend 配置；启用时才是真正意义上的无限等待。
    printf("%s\r\n", s);
    xSemaphoreGive(print_mutex); // 打完一整行才放锁,期间别的任务插不进来
}

// ==================== 演示1:任务骨架/栈溢出实验位 ====================
static void led_task(void *arg)
{

   for (;;) // while(1) 则是有意的任务主循环——调用 vTaskDelay(500) 主动让出 CPU，不是卡死，而是 FreeRTOS 任务的标准写法。
    {
        // die(5); 模拟栈溢出
        vTaskDelay(500);
    }
}

// ==================== 演示2:互斥量(print_task1 vs print_task2) ====================
// 实验方法:把下面两行 print_line 换回 printf,print_task1 把 vTaskDelay 注释掉(满速洪水),
//           观察高优先级 task2 每次醒来如何把 task1 的行拦腰截断(见 task2 注释里的样例);
//           换回 print_line 后整行完整 —— 互斥量的作用。
static void print_task1(void *arg)
{

   for (;;)
    {
        // print_line("11111111111111111111111");
        // printf("11111111111111111111111\r\n");
        // vTaskDelay(100);
    }
}

// 做"每 500ms 精确一次"的闪烁，对比vTaskDelay的漂移
static void print_task2(void *arg)
{
    TickType_t lastWakeTime = xTaskGetTickCount(); // 锚点：进循环前对准一次

   for (;;)
    {
        // print_line("22222222222222222222222");
        // printf("22222222222222222222222\r\n");
        vTaskDelayUntil(&lastWakeTime, 500); // 固定在"上次 + 500ms"醒，不漂移
        //task2 优先级 4 > task1 的 2。不用互斥锁，当task1在打印的时候，task2同时也在打印，会切碎task1
        /*
        像这样
        11111111111111111111111
        11111111111111122222222222222222222222
        11111111
        11111111111111111111111
        11111111111111111111111
        */
    }
}

// ==================== 演示3:LCD 刷屏测试 ====================
/**
 * @brief LCD 刷屏测试任务
 * @param arg 任务参数(未使用)
 * @return 无
 * @note 每 500ms 白/黑全屏交替,用于验证 LCD 驱动
 */
static void lcd_task(void *arg)
{
   for (;;)
    {
        LCD_Clear(0, 240, 0, 320, WHITE);
        vTaskDelay(500);
        LCD_Clear(0, 240, 0, 320, BLACK);
        vTaskDelay(500);
    }
}

// ==================== 演示4:计数信号量(最小 API 全流程) ====================
// 场景:生产端每 1s 一口气 Give 3 次(模拟"3 个事件同时到达");
//       消费端每 300ms 才 Take 一次,故意慢慢消化 → 计数可见地爬坡、泄洪:
//       P +1 cnt=1 → 2 → 3(一口气),然后 C -1 cnt=2 → 1 → 0(每300ms一口)。
// 对比记忆:换成二值信号量(上限1)的话,第 2、3 次 Give 直接失败,事件丢 2/3 ——
//           计数信号量存在的意义就是"事件攒得住,闲了再处理"。
static SemaphoreHandle_t sem_events; // 计数信号量句柄(上限8、初始0)

static void sem_prod_task(void *arg) // 生产端:优先级 3(比消费端高,3次Give一口气打完,不被插队)
{
    int i;
    char buf[24];

    for (;;)
    {
        for (i = 0; i < 3; i++)
        {
            xSemaphoreGive(sem_events); // Give:计数+1(加满上限后再Give返回pdFAIL,不报错)
            snprintf(buf, sizeof buf, "P +1  cnt=%d", (int)uxSemaphoreGetCount(sem_events));
            print_line(buf);
        }
        vTaskDelay(1000); // 每 1s 生产一批
    }
}

static void sem_cons_task(void *arg) // 消费端:优先级 2(故意低于生产端 → 计数才爬得上去)
{
    char buf[24];

    for (;;)
    {
        xSemaphoreTake(sem_events, portMAX_DELAY); // Take:计数-1;为0则阻塞睡大觉,有人Give才醒
        snprintf(buf, sizeof buf, "C -1  cnt=%d", (int)uxSemaphoreGetCount(sem_events));
        print_line(buf);
        vTaskDelay(300); // 每 300ms 才处理下一个 → 泄洪的速度肉眼可见。消费端的速度大于生产端的速度
    }
}

// ==================== 任务启动编排 ====================
/*xTaskCreate(
TaskFunction_t      pxTaskCode,    // ① 任务函数指针
const char * const  pcName,        // ② 任务名字（仅调试用）
configSTACK_DEPTH_TYPE usStackDepth, // ③ 栈深度（单位：字！）32位系统：1字 = 4字节（32位）
void *              pvParameters,  // ④ 传给任务函数的参数
UBaseType_t         uxPriority,    // ⑤ 优先级，区分于stm32的NVIC优先级，FreeRTOS优先级数值越大，任务优先级越高
TaskHandle_t *      pxCreatedTask  // ⑥ 任务句柄（输出参数）
);

从堆（heap_4）分配两块内存：
TCB（任务控制块，内核用 struct tskTaskControlBlock 管理这个任务）+ 任务栈（你说的 128 字）
调用移植层的 pxPortInitialiseStack 把任务栈预铺成"刚被中断打断的样子"——里面预置了 xPSR、PC（指向你的任务函数）、LR（指向 prvTaskExitError）、R0（参数）——这样第一次"切进去"就像恢复一次中断上下文
把 TCB 链进就绪链表中优先级 1 的队列
返回
*/

/**
 * @brief 创建所有学习演示任务(与业务无关)
 * @return 无
 * @note 在 vTaskStartScheduler() 之前调用;不想跑的演示注释对应行即可
 */
void learn_tasks_init(void)
{
    print_mutex = xSemaphoreCreateMutex(); // 打印锁:堆分配,必须在调度器启动前创建
    sem_events = xSemaphoreCreateCounting(8, 0); // 计数信号量:上限8、初始0,同样在启动前创建

    xTaskCreate(led_task, "led_task", 128, NULL, 3, NULL);

    xTaskCreate(print_task1, "print_task1", 128, NULL, 2, NULL);

    xTaskCreate(print_task2, "print_task2", 128, NULL, 4, NULL); // 优先级最高，防止在打印的时候，被别的任务抢走 CPU，导致打印的时间戳不准确

    xTaskCreate(lcd_task, "lcd_task", 128, NULL, 2, NULL);

    xTaskCreate(sem_prod_task, "sem_prod", 256, NULL, 3, NULL); // 生产:一口气Give 3次
    xTaskCreate(sem_cons_task, "sem_cons", 256, NULL, 2, NULL); // 消费:300ms才吃一口
}

/*栈 = 任务干活时用的草稿纸。


函数每次调用别的函数（比如 LED_RED_toggle()）、存临时变量，都要在纸上写几笔。纸不够用，写到"本子外头"去了，就叫栈溢出。

FreeRTOS 怎么查溢出
它有个土办法，像煤窑里的金丝雀。

Stack Canary（栈金丝雀/栈保护值）：
发给你一叠纸，先用记号笔在最底下那张上画个记号 0xa5
每 500ms 任务切换（任务睡觉换人）时，翻开来看看记号那张纸有没有被写脏
写脏了 = 你的字迹已经写到底了 = 溢出 → 喊你写的那个 Hook 打印
你踩的坑：栈设成 1 个字 = 只发 1 张纸
但任务"出生"时，FreeRTOS 要假装任务是被中断打断的，得先压 8 个数进栈（就像开机要先铺 8 张底稿）。

问题来了：只有 1 张纸，这 8 个数根本放不进你发的这叠纸里，全写到纸堆下面去了——写的是"桌子"。
*/
