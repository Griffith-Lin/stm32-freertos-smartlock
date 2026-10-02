#include "usart1.h"

#include "usart1.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *                ★ 手撕路线图:从这个文件往下自己写出来 ★
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * 【先看这个文件现在在哪一关】
 *   关卡 0(裸机阻塞收发)  ── 已完成:usart1_send_byte / usart1_rev_byte
 *   关卡 1(中断 + IDLE 判帧)── 已完成:USART1_IRQHandler + usart_flag 轮询
 *   关卡 2(信号量 + 任务)  ── ★ 还没写,这是你现在要撕的
 *   关卡 3(加固)          ── ★ 还没写
 *   所以下面 0/1 只留"当时该验什么"备查,重点在 2 和 3。
 *
 * 【怎么用这份清单】
 *   **每一关都必须"烧录 → 亲眼看到现象"才准进下一关。**
 *   外设 / 中断 / RTOS 三层一次全写上,出问题你根本分不清是哪层的锅 ——
 *   这是手撕最大的坑,不是代码本身难。
 *
 * 【撕什么,不撕什么 —— 先看这个,别把时间花错地方】
 *   ✔ 值得撕(撕完才算真会):
 *       · ISR 里 IDLE 分支末尾那几句:Give + YIELD
 *       · 任务里那 5 行:Take + 处理
 *       · 两者之间的"交接"设计:为什么要拷贝、为什么要判 NULL、为什么要边界检查
 *   ✘ 不值得撕(查手册抄的活,背下来没用,面试也不问):
 *       · GPIO 复用号、USART_InitTypeDef 那几个字段、NVIC_InitTypeDef 怎么填
 *       · 波特率怎么算 —— 库函数内部算好了,知道"传 115200 进去"就够
 *   省下来的时间,全砸在关卡 2 的优先级上。
 *
 * ── 关卡 0:裸机阻塞版(已完成,留作备查)──────────────────────────
 *   验证过的标准:串口助手敲一个字符,立刻回显一个字符。
 *   翻车对照:
 *     一个字都收不到 → 时钟没开 / AF 号写错 / TX-RX 没交叉接(要 TX 接 RX)
 *     全是乱码       → 波特率或主频不对(先确认 HSE 起振,再查 PLL)
 *
 * ── 关卡 1:中断 + IDLE 判帧(已完成,留作备查)────────────────────
 *   验证过的标准:发 "hello" 然后**停顿一下**,能完整回显 "hello"。
 *   翻车对照(这三条几乎人人撞一遍):
 *     只回显最后一个字符 → 每收一字节都触发一次 IDLE,帧被切碎了
 *     永远不回显         → IDLE 根本没触发。它靠"总线空闲满一个字节时间",
 *                          串口助手发完不停顿(或连续粘发),就永远等不到空闲
 *     死机 / 跑飞        → RXNE 分支漏了读 DR → 标志位清不掉 → 中断风暴
 *
 * ── 关卡 2:把"轮询 usart_flag"换成"信号量"(★ 从这里开始撕)──────
 *   目标:让主循环从 while(!usart_flag); 变成"睡到有人叫才醒"。
 *
 *   □ 先确认 FreeRTOSConfig.h 这三个值(本工程实测值,改错一个就静默卡死):
 *       configMAX_SYSCALL_INTERRUPT_PRIORITY = 0xB0   ← 高 4 位 = 11
 *       configPRIO_BITS                      = 4
 *       INCLUDE_vTaskSuspend                 = 1
 *
 *   □ □ □ 把 USART1 抢占优先级从 3 改成 15 □ □ □
 *       —— 本文件 usart1_Config 里现在填的就是 3,**不改必卡死**。
 *       为什么:ISR 里要调 xSemaphoreGiveFromISR,而 FreeRTOS 规定
 *       只有"内核用 BASEPRI 屏蔽得动"的中断才能调这类 API;门槛是
 *       configMAX_SYSCALL_INTERRUPT_PRIORITY,等价于"抢占优先级数值 >= 11"。
 *       注意 Cortex-M 数值越小越紧急,所以 3 属于"比门槛还紧急" → 内核管不住它,
 *       它却去动内核链表 → configASSERT 命中,而本工程的 configASSERT 是
 *       "关中断 + 死循环":串口再无输出,系统静默卡死,复位都救不回来。
 *       代价:改成 15 后它也落进"内核能屏蔽"的区间,响应延迟变大。
 *             真正要求极低延迟、且绝不碰 FreeRTOS API 的中断,反而要故意留在 0~10。
 *
 *   □ 建全局句柄:SemaphoreHandle_t usart_rx_sem = NULL;
 *       (初值必须 NULL —— 下面那句判空就靠它)
 *
 *   □ ISR 的 IDLE 分支末尾,照抄这三句(**顺序不能换**):
 *       BaseType_t xWoken = pdFALSE;
 *       xSemaphoreGiveFromISR(usart_rx_sem, &xWoken);
 *       portYIELD_FROM_ISR(xWoken);
 *       记牢:xWoken 是"回执单",每次调用前都要自己重新置回 pdFALSE ——
 *       内核只会把它置真,不会替你清零。
 *
 *   □ 写 usart_rx_task:for(;;) { Take 信号量 → print_line(rx_frame); }
 *       平时睡在 xSemaphoreTake(..., portMAX_DELAY) 上,不占 CPU。
 *
 *   □ usart1_tasks_init:xSemaphoreCreateBinary() + xTaskCreate()
 *       任务优先级 4(configMAX_PRIORITIES=5 → 合法 0~4),栈 256 字。
 *       必须在 vTaskStartScheduler() **之前**调 —— 信号量要从 heap_4 分配,
 *       堆一共 8KB,那时没人跟你抢。
 *
 *   □ main 顺序:usart1_Config → 其它初始化 → usart1_tasks_init → vTaskStartScheduler
 *
 *   验证:回显正常;而且**别的任务还在正常跑**,没被串口任务饿死。
 *   翻车对照:
 *     进不了串口任务,系统静默卡死,串口再无输出 → 优先级还是 3,撞了 configASSERT
 *     回显慢一拍(要等下一个 tick 才出来)      → 漏了 portYIELD_FROM_ISR
 *     上电第一个字节到达时就崩                  → 信号量还没建就收了,
 *                                                  GiveFromISR(NULL) 撞 assert
 *
 * ── 关卡 3:加固(把"能跑"变成"跑不坏")──────────────────────────
 *   这四条都是你在关卡 2 里迟早会撞上的,提前知道能省半天:
 *
 *   □ 边界检查: i < sizeof(str_buf)-1 才写;超长时**照样要把 DR 读走**
 *                (不读 → RXNE 一直置位 → 中断风暴,和关卡 1 那条同源)
 *                现状:本文件 str_buf[i++] 是无条件写的,连发超 99 字节就写穿,
 *                      静默踩坏相邻全局变量 —— 现象极难查
 *
 *   □ 交接区:   IDLE 时先把整帧拷进一个 rx_frame,再 Give
 *                (不拷的现象:回显被截断、或前后两帧粘一起,
 *                 而且**随发送速度时有时无** —— 这种"偶发"最难查。
 *                 根因:任务还没打印完,下一帧就从 str_buf[0] 开始覆盖了)
 *
 *   □ 判空:     usart_rx_sem != NULL 才 Give
 *                (不判的现象:usart1_Config 里就开了中断,而信号量要等到
 *                 usart1_tasks_init 才建,中间隔着 LCD_Init 那几百毫秒 ——
 *                 这期间来一个字节就死)
 *
 *   □ 打印:     走 print_line 而不是 usart1_send_string —— 串口是全局共享资源,
 *                得和别的打印任务共用一把锁,理由见 api/debug.c
 *
 * ── 验收标准(达不到 = 还没撕会)────────────────────────────────
 *   ① 清空 USART1_IRQHandler 和 usart_rx_task 的函数体,不看任何参考能默写出来
 *   ② 每一行都能说出"为什么它在这":为什么中断里不打印?为什么非要拷一次 rx_frame?
 *      为什么优先级非 15 不可?
 *   ③ **故障注入**(最能说明问题的一步):故意把优先级改回 3 → 你预测"卡死";
 *      故意删掉 rx_frame 拷贝 → 你预测"回显截断"。**预测对了才算真懂** ——
 *      因为注释里写的是结论,能预测才说明推理链长在你脑子里了。
 * ═══════════════════════════════════════════════════════════════════════════ */

void usart1_Config(uint32_t brr)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);

    GPIO_InitTypeDef gpio_InitTypeDef = {0};

    gpio_InitTypeDef.GPIO_Mode = GPIO_Mode_AF;
    gpio_InitTypeDef.GPIO_OType = GPIO_OType_PP;
    gpio_InitTypeDef.GPIO_Speed = GPIO_Low_Speed;
    gpio_InitTypeDef.GPIO_PuPd = GPIO_PuPd_NOPULL;
    gpio_InitTypeDef.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_10;

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_USART1);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_USART1);
    GPIO_Init(GPIOA, &gpio_InitTypeDef);

    USART_InitTypeDef usart_InitTypeDef;

    usart_InitTypeDef.USART_BaudRate = brr;                                       // 库函数内部自动计算 div_m 和 div_f
    usart_InitTypeDef.USART_WordLength = USART_WordLength_8b;                     // CR1 &= ~(1<<12) -> 8位
    usart_InitTypeDef.USART_StopBits = USART_StopBits_1;                          // CR2 &= ~(3<<12) -> 1位停止位
    usart_InitTypeDef.USART_Parity = USART_Parity_No;                             // CR1 &= ~(1<<10) -> 无校验
    usart_InitTypeDef.USART_HardwareFlowControl = USART_HardwareFlowControl_None; // 无流控
    usart_InitTypeDef.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;                 // CR1 |= (3<<2) -> 收发使能

    NVIC_InitTypeDef NVIC_InitStructure;
    NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 15; // 抢占优先级。必须比FRConfig.h中规定的fromISR API的优先级要低
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;         // 子优先级

    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;

    NVIC_Init(&NVIC_InitStructure);

    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE); // 接收中断使能
    USART_ITConfig(USART1, USART_IT_IDLE, ENABLE); // 空闲中断使能

    // 初始化 USART1
    USART_Init(USART1, &usart_InitTypeDef);

    // 使能
    USART_Cmd(USART1, ENABLE);
}

void usart1_send_byte(uint8_t byte)
{
    while (!USART_GetFlagStatus(USART1, USART_FLAG_TXE))
        ; // 等待接收标志位

    USART_SendData(USART1, byte);
}

uint8_t usart1_rev_byte(void)
{
    uint8_t byte;
    while (!USART_GetFlagStatus(USART1, USART_FLAG_RXNE))
        ;
    byte = USART_ReceiveData(USART1);
    return byte;
}

// 创建一个信号量句柄
SemaphoreHandle_t usart_rx_sem = NULL; // 中断 → 任务的通知信号量

volatile uint8_t usart_flag = 0;
volatile uint8_t str_buf[100];    // 用来接收串口数据
volatile uint8_t rx_str_buf[100]; // str_buf[100]的交接区，打印期间，str_buf[100]被被新值覆盖，不会影响本该要打印的值

void USART1_IRQHandler(void)
{
    static uint16_t i = 0;
    if (USART_GetITStatus(USART1, USART_IT_RXNE)) // 接收中断
    {
        USART_ClearITPendingBit(USART1, USART_IT_RXNE); // 清除标志位

        // 越界检查
        if (i < sizeof(str_buf) - 1)
        {
            str_buf[i++] = USART_ReceiveData(USART1);
        }
        else
        {
            (void)USART_ReceiveData(USART1); // MISRA C 里就有这么一条(Rule 17.7:非 void 函数的返回值必须被使用)。读DR，清RXNE（收满一个字节,DR 里有货等你取）
        }
    }

    if (USART_GetITStatus(USART1, USART_IT_IDLE)) // 空闲中断
    {
        volatile uint32_t temp;
        temp = USART1->SR;
        temp = USART1->DR;
        (void)temp;

        str_buf[i] = '\0';

        // 搬进交接区
        for (uint16_t k = 0; k <= i; k++)
        {
            rx_str_buf[k] = str_buf[k];
        }

        i = 0;
        usart_flag = 1;

         // 确保信号量usart_rx_sem在被usart1_tasks_init()建出来之前，不要碰它
    if (usart_rx_sem != NULL)
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE; // 创建一个空白回执

        xSemaphoreGiveFromISR(usart_rx_sem, &xHigherPriorityTaskWoken); // 放一个信号量，接收回执。（回执用来记录有没有任务被真的唤醒。但为什么可能没人被唤醒才是关键:二值信号量深度只有 1。上一帧任务还没取走时,这次 Give 直接返回 pdFAIL,谁也没醒,回执保持 pdFALSE。）。
        /*
        从 Give 到 Take,大约 1~2 微秒(退栈 + 上下文切换的开销)。

        第二帧要在这个 1~2µs 的窗口里到达,才会被丢。 可一帧最短也是 1 个字节 = 87µs @115200(每一字节 10 位,11520 字节/秒)。差了 40 倍以上。

        所以:连发两帧,第二帧几乎不会被丢。 我那句话是错的。

        真正会丢的是第三帧往后——因为那时任务正在打印:


        帧2 的 IDLE 到达 → Give(sem=1)   ✓ 记上账
        帧3 的 IDLE 到达 → Give 失败 ✗    sem 已经是 1 了,没处记账
        帧4 ...          → 继续丢
        任务打印完帧1 → Take → 拿到的是帧4 的内容
        回显 99 字节要 8.6ms,这 8.6ms 里能塞进来 100 帧。丢帧的窗口 = 任务处理一帧的耗时,不是"连续两帧"。

        那为什么还用这套???
        因为你的对手是人。

        串口助手后面坐着一个人,打字间隔是几百毫秒到几秒。而这个方案开始丢帧的门槛是"帧间隔 < 任务处理一帧的时间",在回显场景下是 8.6ms 量级。人的手速离这个阈值差了三个数量级。

        换句话说:这个缺陷是真实存在的,但它在你的使用场景之外。 方案本身没写错,是取舍——用"高频连发会丢"这一条,换来了:

        任务平时 0% CPU(睡在 Take 上)
        中断只有几微秒(读 DR + 存 + Give)
        代码十几行,没有缓冲区管理
        对"人敲串口助手回显"这个需求,这是笔很划算的交易。
        */

        portYIELD_FROM_ISR(xHigherPriorityTaskWoken); // 挂一个待办事项：如果被唤醒的任务优先级严格高于当前被打断的任务，等中断正常退栈后，让刚被叫醒的高优先级任务立刻拿到cpu
    }
    }

   
}



// 串口接收任务(消费者):拿到信号量就把整帧回显出去
static void usart_rx_task(void *arg)
{
    for (;;)
    {
        if (xSemaphoreTake(usart_rx_sem, portMAX_DELAY) == pdPASS)//拿到信号量
        {
            print_line((const char *)rx_str_buf);
        }
    }
}

/*
所以 vTaskDelay 到底什么时候才需要
一条判据:

for(;;) 里需不需要 vTaskDelay,取决于循环体里有没有"会阻塞的调用"。

循环体	要不要 delay
有 xSemaphoreTake / xQueueReceive / xEventGroupWaitBits	不要
纯计算、纯轮询标志位、纯读写 IO	必须加
你这个循环属于第一类,xSemaphoreTake(..., portMAX_DELAY) 自己就是那个阻塞点,它已经替你把 CPU 让出去了,不需要再补一个 delay。


在这里加 vTaskDelay 会怎样?

xSemaphoreTake(...);
print_line(...);
vTaskDelay(10);      // ← 画蛇添足
vTaskDelay 把任务挂进的是延时链表,不是信号量的等待链表。这 10ms 里它根本没在听那个信号量。此时中断 Give 一下,信号量计数变成 1,但没人被唤醒——门铃响了,屋里没人。

等 10ms 到了任务才回去 Take,发现有个信号量,取走、打印。帧不会丢(计数还在),但这一帧被白白拖了最多 10ms。

所以在这里加 delay 是纯亏:延迟变长,一点好处没有。
*/



// 串口模块的RTOS部分初始化：建信号量+创建接收任务
void usart1_tasks_init(void)
{
    usart_rx_sem = xSemaphoreCreateBinary(); // 创建出来是"空"的,第一次 Give 之前 Take 会一直阻塞

    xTaskCreate(usart_rx_task, "usart_rx", 256, NULL, 4, NULL);
}



/* ═══════════════════════════════════════════════════════════════════════════
 *        进阶:用环形缓冲,把"会丢帧"变成"一帧不丢"(看懂思路就行,先别急着写)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * 【先纠正一个直觉】丢帧的锅不在信号量,在"仓库只有一个格子"
 *   二值信号量只是门铃,它被合并了不丢数据 —— 因为数据根本不在它手里。
 *   真正丢数据的是 rx_str_buf 只有一份:任务还没打印完,新帧就盖上来了。
 *   所以要解决的不是"通知",是"存不下"。解法:让数据也排队。
 *   ★ 一句话:信号量管"喊",环形缓冲管"存"。想一帧不丢,得给"存"加容量。
 *
 * ── 第一步:决定缓冲区的"元素"是什么 ────────────────────────────
 *   方案 A 按字节存:缓冲里是连续字节流,任务自己找帧边界
 *                   → 还得再维护"帧从哪到哪"的元信息,复杂
 *   方案 B 按帧存(推荐):一个槽 = 一整帧 → 简单。下面全按 B 讲
 *
 * ── 第二步:结构定义(放全局,别 malloc)─────────────────────────
 *   #define RX_Q_DEPTH  4
 *   uint8_t  rx_ring[RX_Q_DEPTH][100];   // 4×100 = 400 字节
 *   volatile uint16_t rx_w = 0;          // 写指针 —— 只被 ISR 改
 *   volatile uint16_t rx_r = 0;          // 读指针 —— 只被任务改
 *   volatile uint32_t rx_overrun = 0;    // 满而丢弃的帧数,给调试器看
 *
 *   为什么用 uint16_t 而不是 uint8_t/uint32_t:槽数很少,16 位够;
 *   更关键是 Cortex-M4 对**对齐的 16 位读写是一条指令**,天然原子 ——
 *   不会出现"任务读到改了一半的指针"。这个性质是下面第四步的前提。
 *   两个都必须 volatile:它们跨了两个上下文,编译器不许缓存到寄存器里。
 *
 * ── 第三步:环形缓冲只有两个公式 ────────────────────────────────
 *   前进一格:  w = (w + 1) % RX_Q_DEPTH;
 *   等价写法:  if (++w >= RX_Q_DEPTH) w = 0;     ← ISR 里推荐这个
 *
 *   为什么 ISR 里避开 %:DEPTH 不是 2 的幂时,% 会被编译成除法取余,
 *   在 M4 上几十个周期;if 判断只要两三条指令。
 *   (非要写 % 也行,那就把 DEPTH 取成 2/4/8,编译器会自动优化成位与)
 *
 * ── 第四步:★ 环形缓冲凭什么不用加锁 ────────────────────────────
 *   核心洞察:**读指针只有任务写,写指针只有 ISR 写** —— 一人管一个,
 *   谁都不用"读-改-写"对方那个变量。于是整个结构**不需要临界区**。
 *   这就是"单生产者-单消费者"(SPSC),ISR↔任务之间最划算的结构。
 *
 *   对比:如果两边都去改同一个"当前有几帧"的计数器,就必然要进临界区、
 *   配合 BASEPRI 屏蔽 —— 中断因此变长。环形缓冲用"各管各的指针"绕开了。
 *
 * ── 第五步:判空判满 —— 牺牲一个槽位 ────────────────────────────
 *   空:  r == w
 *   满:  (w + 1) % DEPTH == r        ← 注意是"下一格等于读指针"
 *
 *   为什么满不能也写成 r == w:那样"空"和"满"长得一模一样,分不出来。
 *   解法 (a) 牺牲一格(上面这种,最省事)
 *        (b) 另加一个 count 变量 —— 但它两边都要写,又回到要加锁的老路
 *   所以 DEPTH=4 实际只能存 3 帧;要存 4 帧就写 5。
 *
 * ── 第六步:ISR 侧 —— 入队(替掉现在"拷进 rx_str_buf"那一段)──────
 *   str_buf[i] = '\0';
 *   uint16_t next = rx_w + 1;  if (next >= RX_Q_DEPTH) next = 0;
 *   if (next != rx_r)                                 // 没满才入队
 *   {
 *       memcpy(rx_ring[rx_w], str_buf, i + 1);        // ← 先写数据
 *       rx_w = next;                                  // ← 再动指针
 *   }
 *   else
 *   {
 *       rx_overrun++;                                 // 满了:记一笔
 *   }
 *   i = 0;
 *   ...Give + YIELD(和现在一样,不用改)
 *
 *   ★ 顺序为什么绝对不能反:先动指针的话,任务可能立刻看到"有新帧了",
 *     可数据还在搬的途中 —— 它读到的就是半截帧。**数据先落地,指针后动。**
 *   ★ 满了怎么办?上面选的是"丢新帧、保旧帧"。反过来覆盖最老的也行
 *     (任务总看到最新的)。**关键不是选哪个,而是必须选一个并且记数** ——
 *     静默丢数据是最难查的一类 bug,而 rx_overrun 让它变成可观测的。
 *   ★ 小坑:str_buf 声明成了 volatile,而 memcpy 形参是 const void *,
 *     直接传会报"丢弃 volatile 限定符"(和你之前在 usart1_send_string
 *     上撞的警告同源)。最干净的解法:str_buf 只在 ISR 里被碰(写它在
 *     RXNE 分支、读它在 IDLE 分支),任务从不直接看它 ——
 *     所以它**根本不需要 volatile**,去掉即可。真正需要的是 rx_w/rx_r。
 *
 * ── 第七步:任务侧 —— 出队。★ 关键是"抽干",不是"取一帧" ─────────
 *   for (;;)
 *   {
 *       xSemaphoreTake(usart_rx_sem, portMAX_DELAY);   // 等门铃
 *       while (rx_r != rx_w)                           // ★ while,不是 if
 *       {
 *           print_line((const char *)rx_ring[rx_r]);
 *           if (++rx_r >= RX_Q_DEPTH) rx_r = 0;
 *       }
 *   }
 *
 * ── 第八步:★★ 最重要的一课 —— 信号量在这里降级成了"纯门铃" ──────
 *   盯住第七步那句 `while (rx_r != rx_w)`。它是 **while 而不是 if**,
 *   这一个字的差别,就是"会丢帧"和"一帧不丢"的分界线。
 *
 *   原理:它把"信号的丢"和"数据的丢"彻底拆开了。
 *     · 信号量被合并(门铃没响第二声)→ 任务醒来一看 rx_r != rx_w,
 *       发现仓库里还有货,接着搬 → **通知丢了,不影响数据**
 *     · 任务正在打印时新帧到达 → 插进环形缓冲排着队,等下一轮被搬
 *   数据安不安全,只取决于"仓库满没满",和"门铃响了几次"完全无关。
 *
 *   【一句话】信号量当门铃,环形缓冲当仓库。门铃可以听漏,仓库不会丢货。
 *
 * ── 代价(没有白吃的午饭)──────────────────────────────────────
 *   · RAM:DEPTH × 100 字节。DEPTH=4 → 400 字节,想多存就线性涨。
 *   · 代码:多了约 20 行指针管理,回绕、判满、越界都得自己写对。
 *   · 调试:出问题时"任务打印的是第几帧"不再一眼可见,得盯两个指针。
 *   · 多一次 memcpy(ISR 把 str_buf 搬进 rx_ring),ISR 因此长一点点
 *     —— 100 字节大约几百个周期,微秒级,可以接受,但不是零成本。
 *
 * ── 一个更省事的替代:直接用 FreeRTOS 队列 ──────────────────────
 *   xQueueCreate(RX_Q_DEPTH, 100) + xQueueSendFromISR + xQueueReceive:
 *   queue.c 内部**本来就是环形缓冲**,临界区、阻塞唤醒全替你处理好了,
 *   连信号量都省了(队列自带阻塞)。
 *   代价:从 heap_4 分配(你一共 8KB 堆);且 Send/Receive 各拷一次,
 *   比手写多一次 memcpy。
 *
 *   【怎么选】想搞明白"这东西到底怎么转起来"→ 手写一遍(就是上面这套);
 *             生产代码图省事、不差那点 RAM → 用队列。
 *             两条路都走过,才算真的会。
 * ═══════════════════════════════════════════════════════════════════════════ */























// 串口从上位机接收不规范字符串(....\r\n)
// 调用者传的 buffer 可能是 uint8_t buff[50]，但函数不知道，发 200 字节就溢出了,最好再接收一个长度max_len
void usart1_rev_string(uint8_t *buffer)
{
    while (1)
    {
        *buffer = usart1_rev_byte();

        if (*buffer == '\n')
        {
            break;
        }

        buffer++;
    }

    *(buffer + 1) = '\0';
}

// 串口1发送字符串
void usart1_send_string(volatile uint8_t *str) // 刚刚没有写volatile修饰 uint8_t *str，编译器警告了。传参时 volatile uint8_t * → uint8_t *，编译器认为你丢掉了 volatile 修饰，这是在提醒你："这个变量我本来每次都要从内存读的，传进去之后函数可能把它优化成寄存器缓存，你不怕吗？"
{
    while (*str != '\0')
    {
        usart1_send_byte(*str);
        str++;
    }
    usart1_send_byte('\0'); // 再发送一个'\0'规范发送出的字符串
}

// 串口1接收规范字符串(....\0)
void usart1_rev_string_norm(uint8_t *buffer)
{
    while (1)
    {
        *buffer = usart1_rev_byte();

        if (*buffer == '\0')
        {
            break;
        }

        buffer++;
    }
}
