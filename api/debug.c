#include "debug.h"
#include <stdio.h>

/* ==================== 串口打印锁 ====================
 * 串口是**全局共享资源**,不只 print_task1/print_task2 在用,串口接收任务也要回显。
 * printf 经 fputc 逐字符写 USART1->DR,一旦被高优先级任务抢断,会造成两种破坏:
 *   ① 穿插:两行字符互相切碎
 *   ② 丢字符(更隐蔽):task A 查完 TXE 被抢走,task B 写完 DR,A 回来又写一次
 *      → B 那个字节被覆盖丢失(USART 的 DR 一次只存 1 个待发字节)
 * 所以锁的粒度必须等于"一整行"这个原子单位,锁要包住整个 printf。
 * 加在 fputc 里挡不住 —— 那样每个字符都是一段合法临界区,B 照样能插进 A 的行中间。
 *
 * 放在 debug.c 而不是 main.c:谁想打印就包含 debug.h,不必反过来依赖 main.c。
 */
#define PRINT_LOCK_ENABLED 1 // 实验开关:0=无锁对照组 1=加锁。两轮之间只改这一个值,其余全不动

/* ── SemaphoreHandle_t print_mutex ───────────────────────────────────────
 * 和 usart1.c 里的 usart_rx_sem 是同一种句柄类型,区别在"哪种信号量":
 *   usart_rx_sem 是二值信号量 —— 纯通知,谁 Give 都行
 *   print_mutex  是互斥量     —— 有"主人",只有持有者能 Give
 * 为什么打印必须用互斥量而不是二值信号量?见下面 xSemaphoreCreateMutex 那段。
 * 放 static:只有 debug.c 内部拿得到它 → 外部想绕开锁直接 printf 都没门。
 * 【一句话】一把记名的钥匙 —— 存着它,就等于把串口的所有权攥在手里。 */
static SemaphoreHandle_t print_mutex = NULL;

/**
 * @brief 初始化打印锁
 * @return 无
 * @note 必须在 vTaskStartScheduler() 之前调用 —— 互斥量要从 FreeRTOS 堆(heap_4)分配
 */
void debug_init(void)
{
    /* ── xSemaphoreCreateMutex() ─────────────────────────────────────────
     * 建一个互斥量,返回句柄;堆不够时返回 NULL。
     * 它和 xSemaphoreCreateBinary() 长得几乎一样,但多了两条规矩:
     *   ① **只有持有者能 Give**。别的任务想"帮它放"会被内核拒绝(返回 pdFAIL)。
     *      二值信号量没这条 —— 谁 Give 都行,所以它适合"通知",不适合"占坑"。
     *   ② **优先级继承**。低优先级任务握着锁、高优先级任务来抢时,
     *      内核会临时把持有者的优先级顶到和抢锁者一样高,让它赶紧干完放锁 ——
     *      为的是防"中间优先级任务"插队导致的**优先级反转**
     *      (经典事故:1997 年火星探路者号就栽在这上面反复重启,
     *       最后是远程打补丁打开优先级继承才救回来的)。
     *      二值信号量完全没有这个机制。
     * 代价:互斥量比二值信号量重(要多存持有者句柄、多一套继承逻辑);
     *       而且**绝对不能在中断里 Give** —— queue.c 里有专门的 configASSERT
     *       拦这个(中断不是任务,没有优先级可以继承,也没法"持有")。
     * 【一句话】建一把"有记名"的锁:谁锁的谁开,还自带防插队的优先级继承。 */
    print_mutex = xSemaphoreCreateMutex();
}

/**
 * @brief 整行原子打印:凡是往串口写一行,都应走这个函数
 * @param s 要打印的字符串(不含换行,本函数自己补 \r\n)
 * @note 无锁时被高优先级任务拦腰打断 → 串口出现混行/少字符
 *       【实测】把 PRINT_LOCK_ENABLED 置 0 跑过:确实出现了混行和少字符
 *       加锁后 xSemaphoreTake 拿不到就进 Blocked,不占 CPU,直到持有者 Give
 *
 *       这里的 xSemaphoreTake/xSemaphoreGive 和 usart1.c 里是同一对函数,
 *       只是等的对象不同:那边等"中断发来的通知",这边等"轮到我用串口"。
 *       【一句话】Take = 排队领钥匙,领到才往下走;Give = 用完还钥匙,顺手叫醒下一个排队的。
 */
void print_line(const char *s)
{
#if PRINT_LOCK_ENABLED
    xSemaphoreTake(print_mutex, portMAX_DELAY); // 拿不到就睡,不用轮询空耗
    printf("%s\r\n", s);
    xSemaphoreGive(print_mutex);                // 必须放!漏掉 = 其它任务永久阻塞在 Take
#else
    printf("%s\r\n", s);
#endif
}

void font_test(void)
{
    //下载字库
    DMA_Font_Config();
    Font_Load();

    W25Qxx_Read_Bytes(0, font.buff[0], BUFFSIZE); //读取字库
    for(int i = 0; i < BUFFSIZE; i++)
    {
        printf("%02x ", font.buff[0][i]);
    }

    /*
    格式串里那个 0 是"补零标志"，但它只负责把位数补到指定的宽度——你后面没写宽度，所以它什么都不干，输出的是十六进制的最短形式（0 就输出 0）。

    %02x 的三个部分：


    %  0   2   x
    │  │   │   └─ 十六进制小写 (大写用 X)
    │  │   └───── 最小宽度 2 位，不足补零
    │  └───────── 用 0 补位（而不是空格）
    └──────────── 格式说明符开始
    */
}


void die(uint16_t n)
{
    char buf[100];
    buf[0] = (char)n;        // 让编译器没法偷懒把这层纸删掉
    if (n > 0)
        die(n - 1);          // 喊 n 次就一层层回头
}
