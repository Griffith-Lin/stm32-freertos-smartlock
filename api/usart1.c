#include "usart1.h"
#include "debug.h" // print_line:回显要走全局打印锁,不能自己闷头 printf


void usart1_Config(uint32_t brr)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA,ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1,ENABLE);
    
    GPIO_InitTypeDef gpio_InitTypeDef={0};
    
    gpio_InitTypeDef.GPIO_Mode=GPIO_Mode_AF;
    gpio_InitTypeDef.GPIO_OType=GPIO_OType_PP;
    gpio_InitTypeDef.GPIO_Speed=GPIO_Low_Speed;
    gpio_InitTypeDef.GPIO_PuPd=GPIO_PuPd_NOPULL;
    gpio_InitTypeDef.GPIO_Pin=GPIO_Pin_9 | GPIO_Pin_10;
    

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_USART1);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_USART1);
    GPIO_Init(GPIOA,&gpio_InitTypeDef);
    
    
    USART_InitTypeDef usart_InitTypeDef;
    
    usart_InitTypeDef.USART_BaudRate = brr;            // 库函数内部自动计算 div_m 和 div_f
    usart_InitTypeDef.USART_WordLength = USART_WordLength_8b; // CR1 &= ~(1<<12) -> 8位
    usart_InitTypeDef.USART_StopBits = USART_StopBits_1;      // CR2 &= ~(3<<12) -> 1位停止位
    usart_InitTypeDef.USART_Parity = USART_Parity_No;         // CR1 &= ~(1<<10) -> 无校验
    usart_InitTypeDef.USART_HardwareFlowControl = USART_HardwareFlowControl_None; // 无流控
    usart_InitTypeDef.USART_Mode = USART_Mode_Rx | USART_Mode_Tx; // CR1 |= (3<<2) -> 收发使能

    /* ---- 抢占优先级:必须是 15 ----
     * 这个 ISR 里要调 xSemaphoreGiveFromISR(ISR 版的 FreeRTOS API)。
     * FreeRTOS 的规矩:只有抢占优先级 数值 >= configMAX_SYSCALL_INTERRUPT_PRIORITY 的高 4 位
     * 的中断才能调这些 API。你的 FreeRTOSConfig.h 里这个值是 0xB0,高 4 位 = 11。
     * 
     * 一句话:中断要调 FreeRTOS 的 FromISR API,它的**紧急程度**必须不高于
     *        configMAX_SYSCALL_INTERRUPT_PRIORITY(等价于"抢占优先级数值 >= 11")。
     *        注意"高/低"说的是紧急程度 —— Cortex-M 数值越小越紧急,数值上恰好相反。
     *        这个门槛不是"API 的优先级",而是"内核（这里指的是FR的内核）用 BASEPRI 屏蔽得动的边界":
     *        比它更紧急的中断,内核关不掉,也就不能去动内核的数据结构。
     * 
     * 原来的 3 远低于门槛 —— 内核根本屏蔽不掉它,它却去动内核的链表,链表会被撕坏。
     * 改之前是 3 的话,一旦调 GiveFromISR,会当场撞在 port.c 的 vPortValidateInterruptPriority 里
     * 那句 configASSERT(ucCurrentPriority >= ucMaxSysCallPriority),而 configASSERT 被你定义成
     * "关中断 + 死循环":串口再无输出,系统静默卡死。
     * 代价:改成 15 之后它也落进了"内核能屏蔽"的区间,响应延迟会变大。
     *       真正要求极低延迟、且绝不碰 FreeRTOS API 的中断,反而要故意留在 0~10。 */
    NVIC_InitTypeDef NVIC_InitStructure;
    NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 15; // 抢占优先级:必须 >= 11 才能调 FromISR API
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;         // 子优先级
    
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    
    NVIC_Init(&NVIC_InitStructure);

    USART_ITConfig(USART1,USART_IT_RXNE,ENABLE);//接收中断使能
    USART_ITConfig(USART1,USART_IT_IDLE,ENABLE);//空闲中断使能

    // 初始化 USART1
    USART_Init(USART1, &usart_InitTypeDef);
    
    //使能
    USART_Cmd(USART1, ENABLE);
    
}


void usart1_send_byte(uint8_t byte)
{
    while(!USART_GetFlagStatus(USART1,USART_FLAG_TXE));//等待接收标志位

    USART_SendData(USART1,byte);
}

uint8_t usart1_rev_byte(void)
{
    uint8_t byte;
    while(!USART_GetFlagStatus(USART1,USART_FLAG_RXNE));
    byte=USART_ReceiveData(USART1);
    return byte;
}


/* ═══════════════════════════════════════════════════════════════════════════
 *                      USART1 收一帧的完整流程(自上而下)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  串口助手发 "hello",**发完停顿一下**(IDLE 靠"总线空闲"触发,不停顿就永远是半帧)
 *    │
 *    ▼  ① 硬件层
 *  PA10(RX) ──▶ USART1 移位寄存器 ──▶ 每收满 1 字节,触发一次中断  ("hello" = 5 次)
 *    │
 *    ▼  ② 中断上下文 ───────────── USART1_IRQHandler ─────────────
 *  RXNE 置位? ──是──▶ 读 DR 把字节取走 ──▶ str_buf[i++]
 *    │                                     (有边界检查,超长帧丢弃也不会写穿)
 *    │  ← 每来一个字节走一次,重复 5 遍
 *    ▼
 *  IDLE 置位? ──是──▶ 总线空闲满 1 个字节时间 = 一帧收完了
 *    │
 *    ├─▶ str_buf[i] = '\0'
 *    ├─▶ 整帧拷进 rx_frame        ← 交接区:不拷的话,任务还没打印完下一帧就盖上来了
 *    ├─▶ i = 0
 *    └─▶ xSemaphoreGiveFromISR(usart_rx_sem, &woken)//把等着usart_rx_sem信号量的任务唤醒，顺便接收一个是否有人被唤醒的回执给portYIELD_FROM_ISR
 *          │
 *          ▼
 *        portYIELD_FROM_ISR(woken)
 *          └─ 挂起 PendSV:若被唤醒的任务优先级更高,出中断时立刻切,不等下一个 tick
 *    │
 *    ▼  ③ 中断退出 ──▶ PendSV 执行 ──▶ 上下文切换到 usart_rx_task
 *  任务上下文 ───────────────── usart_rx_task ─────────────────
 *    │
 *    │  平时阻塞在 xSemaphoreTake(usart_rx_sem, portMAX_DELAY)  ← 睡在这,不占 CPU
 *    ▼  信号量到手
 *  print_line(rx_frame)
 *    ├─▶ xSemaphoreTake(print_mutex)   ← 串口是全局共享资源,和别的打印任务共用一把锁
 *    ├─▶ printf → fputc → 逐字符写 USART1->DR
 *    └─▶ xSemaphoreGive(print_mutex)
 *    │
 *    ▼  ④ PA9(TX) ──▶ 串口助手显示 hello
 *
 *  ── 一句话记住这张图 ────────────────────────────────────────────────
 *  中断层只干两件事:**搬**(字节搬进缓冲)和**喊**(GiveFromISR)。
 *  真正耗时的活(格式化、等锁、往 DR 里写几百个字符)全推给任务层 ——
 *  这就是"ISR 越短越好"的落地:ISR 短,别的中断才不会被你堵在门外。
 *
 *  另:本 ISR 的抢占优先级必须是 15(>= 11)才够格调 FreeRTOS API,
 *      理由见上面 usart1_Config 里的那段注释。
 * ═══════════════════════════════════════════════════════════════════════════ */

volatile uint8_t usart_flag=0; // 保留给调试器观察用;通知任务的活已经交给 usart_rx_sem
volatile uint8_t str_buf[100];//用来接收串口数据

static volatile uint8_t rx_frame[100]; // 交接区:一帧收完先搬到这,再唤醒任务

/* ── SemaphoreHandle_t usart_rx_sem ──────────────────────────────────────
 * SemaphoreHandle_t 是信号量的"句柄",本质就是个 void*(指向内核里的队列结构体)。
 * 你拿它去 Give/Take,但**不要自己去解引用它** —— 里面的链表归内核管,碰了就崩。
 * 初值 NULL:真正建它的是 usart1_tasks_init(),那之前它就是个空指针,
 * 所以 ISR 里必须先判 NULL 再用(理由见下面 IRQHandler 里那段)。
 * 为什么放全局(而不是函数里的 static):中断和任务分处两个上下文,
 * 必须有个双方都能看见的名字,否则中断给谁?
 *
 * 【一句话】信号量的"取货凭证" —— 拿着它去 Give/Take,但别拆开看里面。 */
SemaphoreHandle_t usart_rx_sem = NULL; // 中断 → 任务的通知信号量

void USART1_IRQHandler(void)
{
    static uint16_t i=0;
    if(USART_GetITStatus(USART1,USART_IT_RXNE))//接收中断
    {
        USART_ClearITPendingBit(USART1,USART_IT_RXNE);//清除标志位

        /* 边界检查:不检查的话,对方连发超过 99 字节(中间没有空闲间隔、不进 IDLE 分支)
         * 就会一直 i++,把 str_buf 写穿,静默踩坏相邻的全局变量。
         * 越界时仍然要把 DR 读走 —— 不读 RXNE 一直置位,会变成中断风暴。 */
        if(i < (uint16_t)(sizeof(str_buf) - 1))
            str_buf[i++]=USART_ReceiveData(USART1);
        else
            (void)USART_ReceiveData(USART1);
    }


    if(USART_GetITStatus(USART1,USART_IT_IDLE))//空闲中断:一帧收完了
    {
        volatile uint32_t temp;
        temp=USART1->SR;
        temp=USART1->DR;
        (void)temp;

        str_buf[i]='\0';

        /* 把整帧搬进"交接区"再唤醒任务。不搬的话:任务还没打印完,
         * 下一帧就从 str_buf[0] 开始覆盖 —— 现象是打印内容被截断或前后两帧粘在一起,
         * 而且随发送速度时有时无。 */
        for(uint16_t k=0; k<=i; k++)
            rx_frame[k]=str_buf[k];

        i=0;
        usart_flag=1;

        /* 信号量要到 usart1_tasks_init() 才建,而 usart1_Config() 一开始就使能了 USART1 中断,
         * 中间隔着 LCD_Init() 等几百毫秒 —— 这期间收到一个字节就会走到这里。
         * 不加这个判断,xSemaphoreGiveFromISR(NULL,...) 会撞 configASSERT 直接卡死。 */
        if(usart_rx_sem != NULL)
        {
            /* ── BaseType_t xHigherPriorityTaskWoken ────────────────────────
             * BaseType_t 是 FreeRTOS 自己定义的类型(32 位机上就是 long),
             * 移植时换架构不用改代码。pdFALSE 就是 0。
             * 这个变量是给下面的 GiveFromISR 当**输出参数**用的"回执":
             * 我们只负责先填 pdFALSE,由内核回头把它改成 pdTRUE。
             * 规矩:每次调用前都要重新置回 pdFALSE —— 内核只会把它置真,不会替你清零。
             *
             * 【一句话】一张空白回执单（用来接收内核回执）:先自己填上"没人被吵醒",内核真叫醒了更急的任务,
             *          就替你改成"有人醒了" —— 下面那句 portYIELD_FROM_ISR 照着它办事， 让刚被叫醒的高优先级任务
             *          立刻拿到 CPU,不必干等到下一个 tick。 */
            BaseType_t xHigherPriorityTaskWoken=pdFALSE;

            /* ── xSemaphoreGiveFromISR(信号量, &回执) ────────────────────────
             * xSemaphoreGive() 的"中断版"。跟任务版只差两点:
             *   ① 没有等待时间参数 —— 中断里不许阻塞,给不成就算了;
             *   ② 多一个回执参数,用来告诉你"这一给,有没有把某个任务从睡着叫醒"。
             * 返回值:pdPASS=给成功 / pdFAIL=给失败。二值信号量深度只有 1,
             *   上一帧任务还没取走时,这一帧的 Give 返回 pdFAIL(不锁存、不排队)。
             *   现象:连发两帧,第二帧被丢掉 —— 这是"通知型"信号量的固有取舍,
             *   它只表达"有事发生",不表达"发生了几次"。要一帧不丢,得换计数信号量,
             *   或者干脆用队列 xQueueSendFromISR 把数据本身搬给任务。
             * 为什么不能自己动手改任务状态、把任务从链表里摘出来?
             *   内核的链表随时可能被任务上下文改到一半,只有走这个 API
             *   (内部自带临界区保护、配合 BASEPRI 屏蔽)才是安全的。
             *
             * 【一句话】在中断里"放行"一个信号量,把等着它的任务叫醒 ——
             *          中断只负责喊人,干活留给任务。 用 xHigherPriorityTaskWoken来接收内核给的回执（有没有人被叫醒？）*/
            xSemaphoreGiveFromISR(usart_rx_sem, &xHigherPriorityTaskWoken);

            /* ── portYIELD_FROM_ISR(回执) ────────────────────────────────────
             * 字面意思:"出了中断就切换"。它是个宏,展开后(见 portmacro.h)就是:
             *     if (回执 != pdFALSE) { ICSR 的 bit28 写 1 = 挂起 PendSV }
             * 回执为真 = 刚被唤醒的任务优先级 ≥ 当前被打断的那个。
             * 为什么只是"挂个待办",不立刻切?
             * 你现在人在 Handler 模式、占着中断的栈,这时候换任务是找死！！！！
             * 所以只给 PendSV 置个 pending 位,
             * 等中断正常退栈、回到线程模式,CPU 才会跳进 PendSV_Handler 真正换栈。
             * 不调它也能跑,代价是:要等到下一个 tick(你这里 1ms)才切换,回显慢一拍;
             * 高频收数时响应会变成锯齿状。
             *
             * 【一句话】中断里的最后一句"出门就换人" —— 让刚被叫醒的高优先级任务
             *          立刻拿到 CPU,不必干等到下一个 tick。 */
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        }
    }
}

/**
 * @brief 串口接收任务(消费者):拿到信号量就把整帧回显出去
 * @param arg 任务参数(未使用)
 * @return 无(永不返回)
 * @note 平时阻塞在 xSemaphoreTake 不占 CPU;中断 Give 一下立刻被唤醒
 *       打印走 print_line 而不是 printf —— 串口是全局共享资源,得和别的任务共用一把锁
 */
static void usart_rx_task(void *arg)
{
    for(;;)
    {
        /* ── xSemaphoreTake(信号量, 最多等多久) ────────────────────────────
         * 取信号量。注意这里是**任务版**;中断里只准用 ...FromISR 版,
         * 两者不能混用(任务版内部会调 vTaskSuspend,在中断里必崩)。
         * 第 2 个参数是超时,单位是 tick:
         *     portMAX_DELAY = 死等       0 = 只探一下,取不到立刻返回
         *     其它值请用 pdMS_TO_TICKS(500) 写,别直接写 500 —— tick 率一改就错。
         * 返回值:pdPASS=取到了 / pdFAIL=超时(传 portMAX_DELAY 时基本不会发生)。
         *
         * 关键在"阻塞"这两个字。取不到时它不是空转轮询,而是把自己从就绪链表摘下来、
         * 挂进这个信号量的等待链表,然后彻底让出 CPU —— 等待期间一个周期都不占。
         * 中断里 Give 的那一刻,内核再把它从等待链表搬回就绪链表。
         * 对比:写成 while(!usart_flag); 就是 100% 占着 CPU 死等,低优先级任务全饿死、
         * 还费电 —— 这正是 RTOS 存在的理由之一。
         * 代价:睡着的这段时间它"叫不醒",除了这个信号量谁也喊不动它。
         * 想边等边响应别的命令,就得改成等队列/事件组,或者设个超时轮流干活。
         *
         * (顺带:你刚把 INCLUDE_vTaskSuspend 开成 1,这有个副作用 —— 超时传
         *  portMAX_DELAY 时内核走"真·无限等待"分支,连超时链表都不挂,
         *  所以这个 Take 一旦等下去,除了 Give 没有第二个办法把它叫回来。)
         * 
         * pdPASS-信号量收到了
         * pdFAIL-信号量没收到
         *
         * 【一句话】"等人送信":没等到就主动睡觉、把 CPU 让出去,一直睡到有人 Give 才醒 ——
         *          任务之间最常用的约定就是"你干完叫我一声"。 */
        if(xSemaphoreTake(usart_rx_sem, portMAX_DELAY)==pdPASS)
        {
            print_line((const char *)rx_frame);
        }
    }
}

/**
 * @brief 串口模块的 RTOS 部分初始化:建信号量 + 创建接收任务
 * @return 无
 * @note 在 vTaskStartScheduler() 之前调用(信号量要从 heap_4 分配)
 *       优先级 4:configMAX_PRIORITIES 是 5(合法值 0~4),给它最高档,收到就立刻回显
 *       栈 256 字:printf 是栈老虎,回显的字符串又有 100 字节,比打印任务留宽一点
 *
 * ══════════ 这套收数方案(空闲中断判帧 + 二值信号量 + 独立任务)的利弊 ══════════
 *
 * 【好处】
 *  ① **不空转**。任务平时睡在 xSemaphoreTake 上,占 0% CPU。
 *     对比"任务里 while(!usart_flag);"那种写法:那是把 CPU 100% 烧在死等上,
 *     低优先级任务全饿死,还费电。
 *  ② **不丢字节**。中断在字节到达的**那一刻**就把 DR 读走,两个字节隔多短都不怕。
 *     对比"任务里 while(!RXNE)"的写法:一旦被高优先级任务抢占超过一个字节时间,
 *     新字节就覆盖了 DR,旧的那个**静默消失** —— 查起来毫无头绪。
 *  ③ **帧边界是硬件白送的**。IDLE 由硬件在"总线空闲满一个字节时间"时自动置位,
 *     协议里不用加 '\n' 之类的结束符,也不用额外开个定时器做超时判帧。
 *  ④ **中断极短**(微秒级)。只干"读 DR + 存数组 + Give"三件事,
 *     别的中断不会被这个 ISR 堵在门外 —— 这就是"ISR 越短越好"的落地。
 *  ⑤ **可调试**。解析、回显全在任务里,能打断点、能单步、能加 vTaskDelay 模拟慢处理;
 *     写在 ISR 里就只能靠翻转 IO 口去猜。
 *  ⑥ **可扩展**。以后要把回显改成命令解析、要支持多帧,
 *     改动全在任务侧,中断那十几行一行不用动。
 *
 * 【坏处】(这些是方案本身的固有代价,不是代码写错了)
 *  ① **会丢帧,而且是静默丢**。二值信号量只表达"有事发生",不表达"发生了几次":
 *     任务还没取走令牌时,第二帧的 Give 直接返回 pdFAIL 被丢掉 —— 连发两帧必丢一帧。
 *     要一帧不丢,得换计数信号量,或者用队列把数据本身搬给任务。
 *  ② **rx_frame 只有一份,而且是覆盖写**。任务正在打印第 N 帧时中断收到第 N+1 帧,
 *     会直接盖上去 —— 现象是回显打到一半突然变成新内容。根治要上双缓冲/环形缓冲,
 *     代价是内存和代码量都翻倍。
 *  ③ **帧边界依赖"对方发完要停一下"**。上位机把两帧粘着连发(中间不足一个字节时间的空闲)
 *     → 被当成**一帧**;对方发一个字节停一下 → 被拆成**好几帧**。
 *     这是 IDLE 判帧的固有前提,怪不到代码头上,但上位机行为一变现象就变。
 *  ④ **99 字节硬上限**。超长帧被边界检查挡住(不会写穿内存),但**尾部数据直接丢弃**,
 *     而且没有任何错误上报 —— 现象是"发长数据只回显前 99 个字符,也不报错"。
 *  ⑤ **多养一个任务**。1KB 栈 + TCB + 信号量,全从 heap_4 里出。
 *     如果只是偶尔收几个字节,这份开销不值 —— 那种场合轮询反而更省。
 *  ⑥ **优先级 4 的代价**。它一醒,所有低优先级任务(连 idle 也算)全得让路。
 *     回显 99 字节 @115200 约 8.6ms,这期间别的任务一步都动不了。
 *
 * 【更稳的方案】(知道有这条路就行,现在不用改)
 *  **DMA + IDLE**:字节由 DMA 硬件直接搬进缓冲区,CPU 完全不参与,
 *  中断次数从"每字节一次"降到"每帧一次"。代价是要理清 DMA 的 NDTR 剩余计数
 *  和缓冲区地址的换算关系,调试难度比现在这套高一档。
 */
void usart1_tasks_init(void)
{
    /* ── xSemaphoreCreateBinary() ────────────────────────────────────────
     * 建一个二值信号量,返回句柄;堆不够时返回 NULL。
     * 它底下就是 xQueueGenericCreate(1, 0, queueQUEUE_TYPE_BINARY_SEMAPHORE):
     *     长度 1  → 所以叫"二值";   每个元素 0 字节 → 它不传数据,只传"有没有事"。
     * (这解释了为什么信号量能当队列用:信号量本来就是"元素长度为 0 的队列"。)
     * 内存从 configTOTAL_HEAP_SIZE 里出,所以必须在 vTaskStartScheduler() 之前调 ——
     * 那时没人跟你抢堆;等调度器跑起来再建,就要考虑并发安全了。
     * 建出来是**空的**(计数=0),第一次 Give 之前谁 Take 谁睡 —— 正是我们要的:
     * 任务一出生就安安静静等着第一帧,不会手贱去打印一段空缓冲。
     * 代价:这里没判 NULL。堆不够时后面 Take 一个空句柄会直接死机。
     * 想要"不占堆"可以换 xSemaphoreCreateBinaryStatic(),代价是自己多写一个
     * StaticSemaphore_t 变量给它当存储。
     *
     * 【一句话】造一个"有没有事发生"的开关:任务 Take 它就一直睡,中断 Give 一下它就醒。 */
    usart_rx_sem = xSemaphoreCreateBinary(); // 创建出来是"空"的,第一次 Give 之前 Take 会一直阻塞

    /* ── xTaskCreate(函数, 名字, 栈深, 参数, 优先级, 拿不拿句柄) ─────────
     *     usart_rx_task : 任务函数。签名必须是 void f(void *),而且**永不返回**
     *                     (真返回了会掉进内核的 prvTaskExitError)。
     *     "usart_rx"    : 任务名,只给调试器/uxTaskGetSystemState 看,别超
     *                     configMAX_TASK_NAME_LEN(12)。
     *     256           : 栈深,单位是**字**不是字节 → 256 字 = 1KB。
     *                     printf 是栈大户,回显的串还有 100 字节,给宽点。
     *     NULL          : 传给任务函数的参数,对应 usart_rx_task 的 arg。用不上就 NULL。
     *     4             : 优先级,数值越大越优先。configMAX_PRIORITIES=5 → 合法 0~4,
     *                     这里给的是最高档。为什么给最高?串口来一帧就该立刻搬走,
     *                     拖久了下一帧会把信号量顶掉(见上面 GiveFromISR 那段)。
     *                     代价:它一醒,所有低优先级任务(连 idle 也算)都得让路。
     *     NULL          : 任务句柄的输出地址。传 NULL = "这张身份证我不要"。
     *                     以后想 vTaskSuspend / vTaskDelete / vTaskPrioritySet 它,
     *                     就必须先建个 TaskHandle_t 变量,把地址传进来接住。
     * 返回值:pdPASS / errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY(堆不够,任务压根没建起来)。
     *         这里没判 —— 真分配失败时 heap_4 会先喊你那个 vApplicationMallocFailedHook
     *         (里面是死循环),根本轮不到你看见返回值。但写给别人用的库代码别这么偷懒。
     *
     * 【一句话】给内核"报户口":交代一个任务干什么活、分多大栈、排第几等,
     *          从此它归调度器托管 —— 你只管写它里面那个 while(1)。 */
    xTaskCreate(usart_rx_task, "usart_rx", 256, NULL, 4, NULL);
}







//串口从上位机接收不规范字符串(....\r\n)
//调用者传的 buffer 可能是 uint8_t buff[50]，但函数不知道，发 200 字节就溢出了,最好再接收一个长度max_len
void usart1_rev_string(uint8_t * buffer)
{
    while(1)
    {
        *buffer = usart1_rev_byte();
        
        if(*buffer == '\n')
        {
            break;
        }
        
        buffer++;
    }
    
    *(buffer+1)='\0';
}

//串口1发送字符串
void usart1_send_string(volatile uint8_t *str)//刚刚没有写volatile修饰 uint8_t *str，编译器警告了。传参时 volatile uint8_t * → uint8_t *，编译器认为你丢掉了 volatile 修饰，这是在提醒你："这个变量我本来每次都要从内存读的，传进去之后函数可能把它优化成寄存器缓存，你不怕吗？"
{
    while(*str!='\0')
	{
		usart1_send_byte(*str);
		str++;
	}
    usart1_send_byte('\0');//再发送一个'\0'规范发送出的字符串
}


//串口1接收规范字符串(....\0)
void usart1_rev_string_norm(uint8_t * buffer)
{
    while(1)
    {
        *buffer = usart1_rev_byte();
        
        if(*buffer == '\0')
        {
            break;
        }
        
        buffer++;
    }
    
    
}







