#include "soft_timer.h"
#include "debug.h"   // print_line:串口是全局共享资源,打印要走全局那把锁
#include <stdio.h>



/* ═══════════════════════════════════════════════════════════════════════════
 *                              软件定时器实验
 * ═══════════════════════════════════════════════════════════════════════════
 * 要验证的一句话:**所有软件定时器共用一个守护任务("Tmr Svc")**。
 * 所以随便哪个回调卡住,别人的定时器全都跟着延误。
 *
 * ── 守护任务("Tmr Svc")是什么 ──────────────────────────────────────────
 * vTaskStartScheduler() 里由内核自动建出来的**一个普通任务** —— 就是用 xTaskCreate
 * 建的,优先级 configTIMER_TASK_PRIORITY,栈 configTIMER_TASK_STACK_DEPTH。
 * 它平时睡在"定时器命令队列"上,闹钟设成"离下一个定时器到期还有多久";
 * 所有定时器的回调都由它一个人执行 —— 所以回调跑在**任务上下文**里,不是中断。
 * 一句话:后台看店的一个任务,唯一的工作就是按时替你调回调。
 *
 * 怎么跑:
 *   ① TIMER_SABOTAGE_ENABLED = 0 烧进去
 *      → [t100] dt=100、[t500] dt=500 一直很稳(偶尔抖 1~3,原因见下面"顺带")
 *   ② 改成 1 再烧
 *      → [t100] 自己变慢是应该的;重点是 **[t500] 的 dt 也不再是 500 了**
 *        这一条才是"共用一个任务"的直接证据 —— 你只坑了一个,另一个也遭殃
 *
 * 顺带能看见的两件事:
 *   - print_task2 的优先级是 4,比守护任务(configTIMER_TASK_PRIORITY = 2)高。
 *     它每次 printf 48 字符约 4.2ms,这期间守护任务被挡在门外 → dt 会抖几毫秒。
 *     这就是软件定时器的**抖动**,它来自调度延迟,不是内核算错了时间。
 *   - 回调里跑的是**任务上下文**,所以这里 Take 锁、printf、vTaskDelay 都没问题。
 *     换成 api/tim.c 的 TIM10 那种硬件定时器中断就绝对不行 ——
 *     那个中断抢占优先级是 1,不到 11,一调 FreeRTOS API 就撞 configASSERT 死循环。
 *
 * 另外:自动重载定时器**会"补课"**。如果因为回调卡住而错过了好几个周期,
 * 内核会把落下的次数一次补齐(连着回调好几次),而不是"跳过就算了"。
 * 所以 ② 里会看到 [t100] 连着打好几行很小的 dt —— 那是补课,不是 bug。
 *
 * ⚠️ 如果串口打出 "stack overflow Tmr Svc",说明守护任务的栈
 *    (configTIMER_TASK_STACK_DEPTH,128 字 = 512 字节)不够 printf 用,
 *    把它调成 256 即可。printf 是栈老虎,这条规律在你三个任务上都验证过了。
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 实验开关:0 = 正常对照组   1 = 故意在 100ms 回调里阻塞 300ms,制造"一个拖垮全部" */
#define TIMER_SABOTAGE_ENABLED 0

//创建定时器句柄
static TimerHandle_t tmr_100ms = NULL;
static TimerHandle_t tmr_500ms = NULL;

static TickType_t t100_last = 0; // 上一次 t100 回调的 tick,用来算间隔
static TickType_t t500_last = 0;

/**
 * @brief 打印一行"我是谁、现在几点、距上次多久"
 * @param name 定时器名字
 * @param p_last 指向该定时器的"上次回调时刻"变量
 * @return 无
 * @note dt 才是重点:正常时它应该是恒定的 100 / 500,乱了就说明被拖了
 *       用无符号减法算 dt,即使 tick 计数器回绕(约 49.7 天),结果依然正确
 */
static void timer_report(const char *name, TickType_t *p_last)
{
    char buf[48];
    TickType_t now = xTaskGetTickCount();
    TickType_t dt  = now - *p_last;
    *p_last = now;

    snprintf(buf, sizeof(buf), "[%s] now=%u dt=%u", name, (unsigned)now, (unsigned)dt);
    print_line(buf);
}

/**
 * @brief 100ms 自动重载定时器的回调
 * @param xTimer 内核传进来的句柄(本函数用不上,靠闭包里的名字区分)
 * @return 无
 * @note 开关打开时故意阻塞 300ms —— 占住的是**守护任务**,不是"我自己的定时器"
 */
static void timer_100ms_cb(TimerHandle_t xTimer)//timers.h的80行，明确规定，定时器回调应该这样写。（没说必须写static）
{
    (void)xTimer;//明确表示用不到
    timer_report("t100", &t100_last);

#if TIMER_SABOTAGE_ENABLED
    /* 故意犯规:在回调里阻塞 300ms。
     * 你以为只坑了自己 —— 但你占的是守护任务,所有定时器共用这一个任务。
     * 观察:改成 1 重烧后,[t500] 的 dt 也不再是稳定的 500。 */
    vTaskDelay(pdMS_TO_TICKS(300));
#endif
}

/**
 * @brief 500ms 自动重载定时器的回调
 * @param xTimer 内核传进来的句柄(未使用)
 * @return 无
 * @note 它自己一行阻塞代码都没有,却会被 100ms 那个回调连累 —— 这就是本实验的意义
 */
static void timer_500ms_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    timer_report("t500", &t500_last);
}

/**
 * @brief 建两个演示用软件定时器并启动
 * @return 无
 * @note 在 vTaskStartScheduler() 之前调用
 *       此时守护任务还没出生,但 xTimerCreate 会先把命令队列建好,
 *       xTimerStart 塞进去的命令会在守护任务起来后被逐条执行 —— 这是官方支持的用法
 */
void soft_timer_demo_init(void)
{
    /* ── xTimerCreate(名字, 周期, 自动重载?, ID, 回调) ──────────────────
     *     名字     : 调试用。调试器看任务/对象列表时能认出来
     *     周期     : 单位是 **tick,不是毫秒**!要写毫秒就用 pdMS_TO_TICKS()。
     *                你 configTICK_RATE_HZ = 1000,所以 1 tick = 1ms。
     *                周期必须 > 0,传 0 会撞 configASSERT 死循环。
     *     自动重载 : pdTRUE  = 周期性,回调完自动重新排回链表
     *                pdFALSE = 一次性,回调完就休眠,想再跑得手动 xTimerStart
     *     ID       : 一个 void*,随便挂个东西。多个定时器共用一个回调时靠它区分
     *                (回调里用 pvTimerGetTimerID(xTimer) 取回来)
     *     回调     : void f(TimerHandle_t)。**跑在守护任务上下文里**,不是中断!
     * 
     * 返回值:句柄;堆不够返回 NULL(这个函数会调 pvPortMalloc)。
     *
     * 两个要点:
     *   ① 建出来是**休眠态**,不 Start 它永远不动。
     *   ② xTimerCreate **不经过命令队列** —— 它在调用者上下文里直接把对象建好。
     *      所以它可以在调度器启动前安全调用,而且是"谁调用谁花时间"。
     *
     * 【一句话】造一个定时器对象 —— 但造出来是停着的,还得再 Start 才跑。 */
    tmr_100ms = xTimerCreate("t100", pdMS_TO_TICKS(100), pdTRUE, (void *)0, timer_100ms_cb);
    tmr_500ms = xTimerCreate("t500", pdMS_TO_TICKS(500), pdTRUE, (void *)1, timer_500ms_cb);

    /* ── xTimerStart(句柄, 最多等多久) ──────────────────────────────────
     * 重点:**这个函数不启动定时器**。
     * 它只是往"定时器命令队列"里塞一张写着"给这个定时器执行 START"的纸条,
     * 然后立刻返回。真正动手的是守护任务 —— 它醒来后才会从队列里取纸条来执行。
     * (整个 timers.h 里 xTimerStart / Stop / Reset / ChangePeriod / Delete
     *  全是这样的宏,背后都是同一个 xTimerGenericCommand。)
     *
     * 第 2 个参数不是"定时器等多久",而是"**纸条塞不进队列时我最多等多久**":
     *     0             = 塞不进就算了(队列长度是 configTIMER_QUEUE_LENGTH = 10)
     *     portMAX_DELAY = 一直等到队列有空位
     * 返回值:pdPASS = 纸条塞进队列了 / pdFAIL = 没塞进去。
     *   注意它说的是"**纸条送没送到**",**不是**"定时器跑没跑起来"。
     *   所以这里返回值不判也没大碍,但你要知道它不保证什么。
     *
     * 为什么调度器还没启动也能调:创建定时器时命令队列就已经建好了,
     * 纸条先躺在队列里,守护任务一出生就会把它取走执行。
     *
     * 【一句话】给守护任务捎个话"开始计时" —— 你只发了张纸条,活是别人干的。 */
    xTimerStart(tmr_100ms, 0);
    xTimerStart(tmr_500ms, 0);
}
