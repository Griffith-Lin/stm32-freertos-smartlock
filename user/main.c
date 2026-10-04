#include "main.h"

#include "FreeRTOS.h"//freertos的核心头文件。  必须第一个包含，其他 FreeRTOS 头文件（task.h、semphr.h 等）都依赖它。它基于 FreeRTOSConfig.h 提供 BaseType_t、TickType_t、pdTRUE/pdMS_TO_TICKS() 这些基础类型和宏。
#include "task.h"//任务。  xTaskCreate() 创建任务、vTaskDelay() 延时、vTaskStartScheduler() 启动调度器、vTaskDelete() 等。
#include "semphr.h"//互斥锁。  xSemaphoreCreateMutex()、xSemaphoreTake()、xSemaphoreGive()、xSemaphoreCreateBinary() 等。因为 FreeRTOS 的信号量/互斥量底层是基于队列实现的，所以它内部会 #include "queue.h"——你包含了 semphr.h 就等于也拿到了队列 API（xQueueCreate、xQueueSend、xQueueReceive）。

/* ==================== 打印压力实验开关 ====================
 * 互斥量实验的"造竞争"开关:print_task1 死循环狂打(低优先级"长工"),
 * print_task2 每 100ms 插一次话(高优先级"插话者"),造出"拦腰打断"的竞争窗口。
 * 实验做完置 0 —— 两个打印任务回到每秒一行的安静状态。
 * 不关掉的话串口会被 111/222 刷屏,串口接收实验的回显根本看不见。
 *
 * 打印锁 print_mutex 和 print_line 已经搬到 api/debug.c:串口是全局共享资源,
 * 别的模块(串口接收任务)也要打印,不能让它们反过来依赖 main.c。
 */
#define PRINT_STRESS_ENABLED 0 // 1=狂打制造竞争(互斥量实验)  0=安静待命(平时)

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
 *       【一句话】任务栈写穿时内核替你打的"急救电话" —— 参数告诉你谁出的事,你负责收尸。
 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    printf("stack overflow %s\r\n", pcTaskName);
    // printf("优先级 = %d\r\n", uxTaskPriorityGet(xTask));  // 查它的优先级

    /* ── vTaskDelete(句柄) ───────────────────────────────────────────────
     * 把一个任务从调度器名册上注销,它占的 TCB 和栈最终会被回收。
     * 传 NULL = 删自己;但删完自己就**不能再碰自己的栈**了(内存已经不属于你),
     * 而且真正的回收要等空闲任务跑起来才发生 —— 所以删自己之后必须立刻让出 CPU。
     * 这里传的是别人的句柄,删完接着 for(;;) 停住,没问题。
     * 【一句话】把任务"开除"掉、内存还给堆 —— 相当于进程版的 kill。 */
    vTaskDelete(xTask); // 或者：把肇事任务直接"枪毙"，让别的任务继续跑
    for (;;)
        ; /* 任务栈溢出（栈配小了或递归太深） */
}


void vApplicationIdleHook(void) // FreeRTOS 空闲任务钩子函数,由 configUSE_IDLE_HOOK 使能。如果有一个任务不再睡觉，那空闲函数就会饿死
{
    // LED_BLUE_toggle();           // 根据led的状态来看进空闲任务的频率

    /* 为了做软件定时器实验先关掉 —— 空闲任务一有空就打印,115200 会被 6666 刷满,
     * 定时器的 [t100]/[t500] 那几行根本看不见。做完实验想恢复就取消注释。
     * (顺带:这行本身就是"空闲任务被饿死/喂饱"的活体指示器,以后还用得上) */
    // print_line("6666666666666666666/r/n");
}


/*
我没给那两个 Hook 里的 printf 加锁：Hook 触发时系统已经不可信，如果某个任务正握着锁，Hook 再去 Take 就是自己等自己的死锁


*/




/*-----------------任务----------------------*/

//挂起流程
// vTaskSuspend(TaskHandle_t xTaskToSuspend);  // 挂起。传 NULL = 挂起自己
// vTaskResume(TaskHandle_t xTaskToResume);    // 恢复
// xTaskResumeFromISR(TaskHandle_t h);         // 中断里恢复,返回 pdTRUE 表示需要切换


static void led_task(void *arg)
{

    while (1) // while(1) 则是有意的任务主循环——调用 vTaskDelay(500) 主动让出 CPU，不是卡死，而是 FreeRTOS 任务的标准写法。
    {
        // die(5); 模拟栈溢出
        /* ── vTaskDelay(节拍数) ─────────────────────────────────────────
         * 把自己从就绪链表摘下来,睡足 n 个 tick 再被搬回去。
         * 你这里 configTICK_RATE_HZ = 1000,所以 500 就是 500ms。
         * 它是"主动让出 CPU"最基本的手段 —— 不调它,这个 while(1) 会把
         * 同优先级和更低优先级的任务全部饿死(空闲任务永远是第一个受害者)。
         * 注意它延的是"从调用这一刻起 n 个 tick",不是"每隔 n 个 tick":
         * 中间被抢占、被中断耽误掉的时间会累积成漂移,周期越走越慢。
         * 要精确周期(比如"每秒整一行"),用它下面的 vTaskDelayUntil。
         * 【一句话】"我先睡 500ms,到点叫我" —— 让出 CPU 又不用自己轮询的标准姿势。 */
        vTaskDelay(500);
    }
}

/**
 * @brief 打印任务1(低优先级"长工")
 * @param arg 任务参数(未使用)
 * @return 无(永不返回)
 * @note 压力模式(PRINT_STRESS_ENABLED=1):48 字符 @115200 8N1 ≈ 4.2ms,必然跨过好几个
 *       1ms tick 边界,而且不让出 CPU —— 给优先级 4 的 print_task2 制造"拦腰打断"的机会。
 *       副作用:idle 任务(优先级 0)会被饿死,正好是"低优先级霸占 CPU 会饿死 idle"的活标本
 */
static void print_task1(void *arg)
{
    while (1)
    {
        // print_line("111111111111111111111111111111111111111111111111");
        
#if !PRINT_STRESS_ENABLED
        vTaskDelay(500); // 平时:每秒两行,把串口让出来
        // 【一句话】和 led_task 里那个是同一个:睡 500ms 再回来,别霸着 CPU 和串口。
#endif
    }
}

/**
 * @brief 打印任务2(高优先级"插话者")
 * @param arg 任务参数(未使用)
 * @return 无(永不返回)
 * @note 优先级 4 > 2,醒来的瞬间立刻抢占(不等下一个 tick),正好砍在 print_task1 的行中间
 *       压力模式周期 100ms,让穿插现象每秒稳定出现约 10 次
 */
static void print_task2(void *arg)
{
    /* ── xTaskGetTickCount() ────────────────────────────────────────────
     * 读内核的节拍计数器:从开机到现在过了多少个 tick。你这里 1 tick = 1ms,
     * 所以这个数约等于"开机多少毫秒"。它的类型是 TickType_t。
     * 在这里当 vTaskDelayUntil 的"锚点":进循环前对准一次,
     * 之后每次唤醒内核都按"锚点 + 周期"推进,误差就不会累积。
     * 【一句话】看一眼内核的秒表 —— 从开机到现在走了多少毫秒。 */
    TickType_t lastWakeTime = xTaskGetTickCount(); // 锚点：进循环前对准一次

    while (1)
    {
        // print_line("222222222222222222222222222222222222222222222222");

        /* ── vTaskDelayUntil(&锚点, 周期) ────────────────────────────────
         * 和 vTaskDelay 的差别就在那个"锚点":
         *   vTaskDelay(1000)         每次"从醒来的这一刻起"再睡 1000 → 误差累积
         *   vTaskDelayUntil(&t, 1000) 内核按 t += 1000 算**绝对时刻** → 每次都对齐
         * 所以它才是做"精确周期"的正确工具。代价:多维护一个 TickType_t,
         * 而且锚点必须初始化好 —— 传个没赋值的进去,任务会疯跑。
         * 它只"等"不"补":万一这一轮活儿干超了周期,它会立刻返回(缺的节拍不补),
         * 现象是紧接着打出两行。锚点由**内核**帮你往前推,你不用自己加。
         * 【一句话】"每隔 1000ms 叫我一次,而且要准时" —— 周期任务的标配。 */
#if PRINT_STRESS_ENABLED
        vTaskDelayUntil(&lastWakeTime, 100);  // 压力模式:每秒插 10 次话,便于观察
#else
        vTaskDelayUntil(&lastWakeTime, 1000); // 平时:每秒一行
#endif
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
    返回值：pdPASS = 建成功；errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY = 堆不够，任务压根没建起来。

    下面 4 个 rc1~rc4 一个都没判。真分配失败时 heap_4 会先喊
    vApplicationMallocFailedHook（里面是死循环），轮不到你看返回值 ——
    但"少了一个任务却没人告诉你"是很坑的现象，正式代码该判还得判。
    （以后想挂起/删除某个任务，就必须把第 ⑥ 个参数从 NULL 换成一个真的 TaskHandle_t 变量
      把它接住 —— 按键挂起 lcd_task 那个实验正要用到。）

    【一句话】给内核"报户口"：交代一个任务干什么活、分多大栈、排第几等，
              从此它归调度器托管 —— 你只管写它里面那个 while(1)。
    */
    BaseType_t rc1 = xTaskCreate(led_task, "led_task", 128, NULL, 3, NULL);

    BaseType_t rc2 = xTaskCreate(print_task1, "print_task1", 128, NULL, 2, NULL);

    BaseType_t rc3 = xTaskCreate(print_task2, "print_task2", 128, NULL, 4, NULL); // 优先级最高，防止在打印的时候，被别的任务抢走 CPU，导致打印的时间戳不准确

    BaseType_t rc4 = xTaskCreate(lcd_task, "lcd_task", 128, NULL, 2, NULL);

    /* 所有 RTOS 对象(队列/信号量/任务)都必须在 vTaskStartScheduler() 之前建好 ——
     * 它们要从 FreeRTOS 堆(heap_4)分配,调度器一跑起来任务可能立刻就用,来不及。
     * (观测:一个互斥量大概占 100 字节) */
    /* ── xPortGetFreeHeapSize() ─────────────────────────────────────────
     * 问内核:FreeRTOS 堆(configTOTAL_HEAP_SIZE 那 8KB)还剩多少字节。
     * 注意它只报"剩余总量",不告诉你碎不碎 —— heap_4 会合并相邻空闲块,
     * 但碎片仍然可能存在。想看历史最低水位用 xPortGetMinimumEverFreeHeapSize()。
     * 这里前后各打一次,差值就是"建这一堆 RTOS 对象花掉了多少"。
     * 【一句话】查内存余额 —— 建对象前看一眼,免得后面默默分不到内存还不知道。 */
    printf("heap before init: %u\r\n", (unsigned)xPortGetFreeHeapSize());
    debug_init();        // 打印锁:print_line 靠它护住整个串口
    usart1_tasks_init(); // 串口接收信号量 + usart_rx 任务
    soft_timer_demo_init(); // 两个演示用软件定时器(100ms / 500ms)
    printf("heap after  init: %u\r\n", (unsigned)xPortGetFreeHeapSize());

    /* ── vTaskStartScheduler() ───────────────────────────────────────────
     * 启动调度器:创建空闲任务(优先级 0),配好 SysTick(1ms 一次),
     * 然后做第一次上下文切换,跳进优先级最高的就绪任务。
     * 正常情况下**永不返回** —— 所以能走到下面那句 "sched returned!",
     * 就说明启动失败了,最常见的原因是堆不够(连空闲任务都没建起来)。
     * 【一句话】按下总开关:从这一刻起 CPU 归调度器分配,不再由你说了算。 */
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
