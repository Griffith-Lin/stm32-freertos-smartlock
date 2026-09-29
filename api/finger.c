#include "finger.h"
#include "MG200.h" // mg200_send / mg200_rec_timeout / check_rec
#include "lock.h"  // lock_open / lock_is_busy / lock_lcd_set_title
#include "lcd.h"   // GREEN / RED / BLUE
#include "FreeRTOS.h"
#include "task.h"
#include "stdio.h"

// ── 参数 ──
#define FINGER_POLL_MS    100  // 空转时轮询 PC0 的周期
#define FINGER_SETTLE_MS  150  // 检测到手指后再等一会,让手指按稳再采图
#define FINGER_TIMEOUT_MS 800  // 等模块单条应答的上限(模块内部比对要几百 ms)
#define FINGER_FAIL_MS    800  // "NO MATCH" 提示停留多久
#define FINGER_RELEASE_MS 50   // 等手指松开时的轮询周期

// 标题行提示语。**必须 8 个字符**:标题行原本是 "PASSWORD"(8 个字符),
// 而 LCD_Font_Dis_Ascii() 只把每个字符格子里的像素写一遍,新文字短了就盖不掉
// 旧的尾巴,右边会留残影(见 lock.h 里 lock_lcd_set_title() 的说明)。
// 所以 "FINGER" 后面补两个空格凑够 8 个;"NO MATCH" 本身就是 8 个。
#define FINGER_TITLE_OK   "FINGER  "
#define FINGER_TITLE_FAIL "NO MATCH"

// ── 管理员模式派下来的活(录入 / 清空)用的参数 ──
#define FINGER_ENROLL_SAMPLES  3       // MG200 注册一枚指纹要采 3 次特征码
#define FINGER_ENROLL_WAIT_MS  15000   // 每次"等手指按下/松开"的上限
#define FINGER_ACK_MS          2000    // 单条命令等模块应答的上限
#define FINGER_ERASE_ACK_MS    5000    // 清空整个库比单条命令慢,给宽一点

// ── 管理员模式派下来的活 ──
//   为什么用标志而不是队列:一次只可能有一件活(管理员模式一次只派一个子模式),
//   而且"后来的请求覆盖先前的"正是想要的行为 —— 用户连按两次 444#,当然只录一次。
//   ★ 跨任务读写:admin(跑在 lock_task 里)写,本任务读。
//     读的时候要"取出并清零"一起做完,见 finger_take_request()。
#define FINGER_REQ_NONE    0
#define FINGER_REQ_ENROLL  1
#define FINGER_REQ_ERASE   2

static volatile uint8_t s_enroll_req;
static volatile uint8_t s_erase_req;

/**
 * @brief  手指是否按在传感器上
 * @note   PC0(DETECT)由模块驱动:空闲为低,检测到手指输出高
 */
#define FINGER_DETECT() (GPIO_ReadInputDataBit(GPIOC, GPIO_Pin_0) == SET)

/**
 * 🟡 L1 架构代码 —— 帧同步的前置条件:上一次没收干净的尾巴留着会和下一次应答拼在一起,让同步从错误的 0x6C 开始;它直接读 DR 清空 RX,和协议层是"谁先读走 RXNE 谁赢"的关系
 * @brief  丢掉 USART6 接收缓冲里的残留字节
 * @note   上一次没收干净的尾巴(校验失败的帧、上电毛刺)如果留在 RX 里,
 *         会和下一次的应答拼在一起,让帧同步从错误的 0x6C 开始。
 *         发命令前清一下,帧同步的成功率高很多。
 */
static void usart6_flush(void)
{
    while (USART_GetFlagStatus(USART6, USART_FLAG_RXNE))
    {
        (void)USART_ReceiveData(USART6);
    }
}

/**
 * 🟡 L1 架构代码 —— 0x51 的 para 是特征码缓冲区号、0x71 才是 1:N 搜索,"result==0 算命中"的契约集中在返回值判断上:任一步判反,现象都是"手指按了但永远开不了锁"
 * @brief  采一次指纹,并在模块的整库中搜索(1:N)
 * @param  id 输出:匹配到的指纹编号(只在返回 1 时有意义)
 * @return 1 = 匹配成功;0 = 采图失败 / 没匹配上 / 模块没应答
 * @note   用的两条命令:
 *           0x51 提取特征码,参数 = 存到哪个缓冲区(这里用 0)
 *           0x71 在指纹库里搜索
 *         "匹配成功"的判断沿用 MG200.c 里 mg200_compare() 的结论:应答的
 *         result == 0。要是实测反过来,只改下面这一处就行。
 */
static uint8_t finger_match(uint8_t *id)
{
    MG_rec_data rec;

    /* 1) 提取特征码 —— 手指必须还按在传感器上,松了就采不到 */
    usart6_flush();
    mg200_send(0x51, 0);
    if (!mg200_rec_timeout(&rec, FINGER_TIMEOUT_MS))
    {
        printf("指纹:采图超时,模块没应答\r\n");
        return 0;
    }
    if (!check_rec(rec))
    {
        return 0; // check_rec() 已经把收到的原始帧打印出来了
    }

    /* 2) 在整库中搜索 */
    usart6_flush();
    mg200_send(0x71, 0);
    if (!mg200_rec_timeout(&rec, FINGER_TIMEOUT_MS))
    {
        printf("指纹:比对超时,模块没应答\r\n");
        return 0;
    }
    if (!check_rec(rec))
    {
        return 0;
    }

    if (rec.result != 0)
    {
        printf("指纹:没匹配上\r\n");
        return 0;
    }

    *id = rec.para; // 数据区:result=0 表示命中,para = 命中的指纹编号
    printf("指纹:匹配成功,编号 %d\r\n", *id);
    return 1;
}

/**
 * 🟡 L1 架构代码 —— "检查 + 清零"必须在同一个临界区里:取出来之后、清零之前对方又置了一次,这次请求就被这次清零吃掉;而这条原子性是"派活不丢"的全部保证
 * @brief  取出并清掉一个待办的录入/清空请求
 * @return FINGER_REQ_NONE / FINGER_REQ_ENROLL / FINGER_REQ_ERASE
 * @note   两件活同时挂着时先做录入、清空留到下一轮 —— 实际上不会同时出现
 *         (111# 只派清空,444# 只派录入),这里只是不想让"同时来"变成丢一件。
 */
static uint8_t finger_take_request(void)
{
    uint8_t req = FINGER_REQ_NONE;

    taskENTER_CRITICAL();
    if (s_enroll_req)
    {
        s_enroll_req = 0;
        req = FINGER_REQ_ENROLL;
    }
    else if (s_erase_req)
    {
        s_erase_req = 0;
        req = FINGER_REQ_ERASE;
    }
    taskEXIT_CRITICAL();

    return req;
}

// 🟢 L2 工具代码 —— 纯置位,没有副作用也没有先后要求;"投递意图而非执行"这条分工由调用方遵守
void finger_request_enroll(void)
{
    s_enroll_req = 1;
}

// 🟢 L2 工具代码 —— 同上;它本身不删任何东西,真正动手的是 finger_erase()
void finger_request_erase(void)
{
    s_erase_req = 1;
}

/**
 * 🟡 L1 架构代码 —— 用"无符号差值"计时而不是比较绝对时刻,是它能跨过 49.7 天 tick 回绕的原因;超时值写错或漏判的现象是任务永久卡在等手指上
 * @brief  等手指按下(或松开),带超时
 * @param  level     1 = 等手指按下去(DETECT 变高);0 = 等手指松开(DETECT 变低)
 * @param  timeout_ms 最多等多久
 * @return 1 = 等到了;0 = 超时
 * @note   为什么不能用 mg200_add_finger() 里那种 `while (GPIO_ReadInputDataBit(...) == SET);`
 *         的写法:那是忙等,任务会一直占着 CPU 不放,同优先级和低优先级的任务
 *         全被饿死。这里每次让出 CPU 睡 FINGER_RELEASE_MS。
 * @note   超时是必须的:用户按了 444# 又走开的话,没有超时就永远不返回,
 *         这个任务再也不会去处理别的事。
 */
static uint8_t finger_wait_detect(uint8_t level, uint32_t timeout_ms)
{
    uint32_t start = xTaskGetTickCount();

    while ((FINGER_DETECT() ? 1 : 0) != (level ? 1 : 0))
    {
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms))
        {
            return 0;
        }

        vTaskDelay(pdMS_TO_TICKS(FINGER_RELEASE_MS));
    }

    return 1;
}

/**
 * 🟢 L2 工具代码 —— 只拼一行 "PUT FINGER n/3",不做任何判断;数字拼错顶多是屏上显示错一位
 * @brief  在大提示区第 2 行显示"放上手指,第 n 次"
 * @param  n 第几次,0 起算
 * @note   手拼字符串而不是用 sprintf:microlib 下 sprintf 的链接行为没验证过,
 *         而这里要拼的内容是固定的两段,手拼更省事也更可控。
 */
static void finger_lcd_step(uint8_t n)
{
    char line[16];
    uint8_t i;

    for (i = 0; i < 11; i++)
    {
        line[i] = "PUT FINGER "[i];
    }
    line[11] = (char)('1' + n);
    line[12] = '/';
    line[13] = (char)('0' + FINGER_ENROLL_SAMPLES);
    line[14] = '\0';

    lock_lcd_big_line(2, line);
}

/**
 * 🔴 L0 生死线代码 —— 每一步的次序都不能动:按稳→采图→松手→下一枚,少一步"等松手"同一根手指会被连采两次、模板作废;末了的 0x7F 不发就只是把特征码丢在缓冲区里,什么都没存
 * @brief  录一枚新指纹(管理员模式 444# 派下来的活)
 * @note   流程和 MG200.c 的 mg200_add_finger() 一样,都是"采 3 次特征码 → 发 0x7F
 *         让模块合成入库",区别只在**每一处等待都有超时、而且会让出 CPU**:
 *         mg200_add_finger() 是给裸机阶段用的,里面有 while(1) 和无超时收帧,
 *         在任务里会永久卡死。
 * @note   这个函数会阻塞 finger_task 若干秒(等用户放手指),但不影响别的任务 ——
 *         锁的电机状态机在 lock_task 里,和这里无关。
 * @note   屏幕抢不到就不显示进度,活照干(见 lock.h 的 lock_screen_take)。
 *         111# 会同时给指纹和刷卡派活,两个都画就会互相盖。
 */
static void finger_enroll(void)
{
    MG_rec_data rec;
    uint8_t n;
    uint8_t got_ui;

    got_ui = lock_screen_take(LOCK_SCREEN_FINGER);

    if (got_ui)
    {
        lock_lcd_big_line(0, "ADD FINGER");
        lock_lcd_big_line(1, "");
        lock_lcd_big_line(3, "KEEP STILL");
        lock_lcd_big_line(4, "");
        lock_lcd_big_line(5, "");
    }

    for (n = 0; n < FINGER_ENROLL_SAMPLES; n++)
    {
        if (got_ui)
        {
            finger_lcd_step(n);
        }

        /* 1) 等手指按下去 */
        if (!finger_wait_detect(1, FINGER_ENROLL_WAIT_MS))
        {
            printf("指纹录入:等手指超时,放弃\r\n");
            if (got_ui)
            {
                lock_lcd_big_line(2, "TIMEOUT");
            }
            break;
        }

        /* 2) 手指刚碰上时位置还没稳,等一下再采 —— 和认证流程同一个道理 */
        vTaskDelay(pdMS_TO_TICKS(FINGER_SETTLE_MS));

        /* 3) 提取特征码。para = 存到哪个缓冲区,三次分别用 0/1/2 */
        usart6_flush();
        if (!mg200_cmd(0x51, n, &rec, FINGER_ACK_MS) || !check_rec(rec))
        {
            printf("指纹录入:第 %d 次采图失败,放弃\r\n", n + 1);
            if (got_ui)
            {
                lock_lcd_big_line(2, "CAPTURE FAIL");
            }
            break;
        }

        /* 4) 等手指松开,否则同一根手指会被连采两次 */
        if (!finger_wait_detect(0, FINGER_ENROLL_WAIT_MS))
        {
            printf("指纹录入:手指一直没松开,放弃\r\n");
            if (got_ui)
            {
                lock_lcd_big_line(2, "RELEASE FINGER");
            }
            break;
        }
    }

    /* 三次都采到了才发 0x7F 合成入库;中途 break 出来的 n 会小于 3,不会走到这里 */
    if (n == FINGER_ENROLL_SAMPLES)
    {
        usart6_flush();
        if (mg200_cmd(0x7F, 0, &rec, FINGER_ERASE_ACK_MS) && check_rec(rec))
        {
            printf("指纹录入:注册成功\r\n");
            if (got_ui)
            {
                lock_lcd_big_line(2, "FINGER SAVED");
            }
        }
        else
        {
            printf("指纹录入:注册失败\r\n");
            if (got_ui)
            {
                lock_lcd_big_line(2, "SAVE FAILED");
            }
        }
    }

    if (got_ui)
    {
        vTaskDelay(pdMS_TO_TICKS(FINGER_FAIL_MS)); // 让结果在屏上停一下
        lock_screen_give(LOCK_SCREEN_FINGER);
    }
}

/**
 * 🔴 L0 生死线代码 —— 不可逆:0x54 一发,模块里所有指纹一次全没,没有任何二次确认;而"抢不到屏就静默执行"那条分支意味着用户可能连提示都看不到
 * @brief  清空指纹库(管理员模式 111# 派下来的活)
 * @note   用 mg200_erase_all_timeout() 而不是 MG200.c 里那个 mg200_erase_all():
 *         后者收帧没有超时,模块一旦不应答这个任务就永久卡死。
 */
static void finger_erase(void)
{
    uint8_t got_ui;

    got_ui = lock_screen_take(LOCK_SCREEN_FINGER);

    if (got_ui)
    {
        lock_lcd_big_line(0, "CLEAR ALL");
        lock_lcd_big_line(1, "");
        lock_lcd_big_line(2, "ERASING...");
        lock_lcd_big_line(3, "");
        lock_lcd_big_line(4, "");
        lock_lcd_big_line(5, "");
    }

    if (mg200_erase_all_timeout(FINGER_ERASE_ACK_MS))
    {
        if (got_ui)
        {
            lock_lcd_big_line(2, "FINGERS CLEARED");
        }
    }
    else
    {
        if (got_ui)
        {
            lock_lcd_big_line(2, "ERASE FAILED");
        }
    }

    if (got_ui)
    {
        vTaskDelay(pdMS_TO_TICKS(FINGER_FAIL_MS));
        lock_screen_give(LOCK_SCREEN_FINGER);
    }
}

/**
 * 🟡 L1 架构代码 —— 业务主流程 + 与密码任务共用 LCD 的互斥策略(lock_is_busy 预检):几段延时/顺序改错会出现同一次按压被反复采图,或两个任务抢着画屏导致花屏
 * @brief  指纹开锁任务
 * @note   平时 100ms 睡一次,只读一个 GPIO,几乎不占 CPU。
 *         检测到手指后的流程:等手指按稳 → 采图 → 比对 → 通过就走开锁流程。
 * @note   指纹和密码两条路径都会写 LCD。LCD 走的是软件模拟 SPI,一次
 *         LCD_Set_Region + 逐点写不是原子操作,理论上两个任务同时画会串。
 *         这里用 lock_is_busy() 把两边错开:正在开锁/回锁时指纹这边不画。
 */
static void finger_task(void *arg)
{
    uint8_t id;
    uint8_t req;

    (void)arg;

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(FINGER_POLL_MS));

        // ── 管理员模式派下来的活优先 ──
        //    ⚠ 这里**不能**加 lock_is_busy() 判断:录入和清空本来就是"用密码开完
        //      锁之后"发生的,那时候锁多半还在开锁/保持/回锁的过程里。
        //      下面那条预检是给"手指碰到就当认证"这条常规路用的。
        req = finger_take_request();
        if (req == FINGER_REQ_ENROLL)
        {
            finger_enroll();
            continue;
        }
        if (req == FINGER_REQ_ERASE)
        {
            finger_erase();
            continue;
        }

        // 没手指,继续睡
        if (!FINGER_DETECT())
        {
            continue;
        }

        // 正在开锁/回锁,这轮不采 —— 采了也会被 lock_open() 拒掉
        if (lock_is_busy())
        {
            continue;
        }

        // 手指刚碰上时位置还没稳,等一下再采图,成功率明显高一些
        vTaskDelay(pdMS_TO_TICKS(FINGER_SETTLE_MS));
        if (!FINGER_DETECT())
        {
            continue; // 手指划过去了
        }

        if (finger_match(&id))
        {
            // 走公共开锁流程。它内部还会再判一次忙,这里不用自己防
            lock_open((uint8_t *)FINGER_TITLE_OK, GREEN);
        }
        else if (!lock_is_busy())
        {
            // 比对这段时间里对方可能已经开始开锁了,那就别去盖它的界面
            lock_lcd_set_title((uint8_t *)FINGER_TITLE_FAIL, RED);
            vTaskDelay(pdMS_TO_TICKS(FINGER_FAIL_MS));
            lock_lcd_set_title((uint8_t *)"PASSWORD", BLUE);
        }

        // 等手指松开,否则同一次按压会被反复采
        while (FINGER_DETECT())
        {
            vTaskDelay(pdMS_TO_TICKS(FINGER_RELEASE_MS));
        }
    }
}

// 🟡 L1 架构代码 —— 栈 256 字是实测定的(任务里要跑 printf 和 MG200 收发),加完它堆只剩 1240 字节:再添任务/队列前必须先读 xFreeBytesRemaining,不够就是 malloc 失败钩子里的死循环
void finger_tasks_init(void)
{
    // 栈 256 字(1KB):任务里会调到 printf 和 MG200 的收发函数,留够余量。
    //
    // 堆余量(实测,不是估算):heap_4 总共 8192 字节,加完这个任务之后剩下
    // 1240 字节。heap_4 的记账变量 xFreeBytesRemaining 在 0x20000038,
    // 调试时挂起内核读一下就能看到还剩多少 —— 以后再往里加任务/队列,
    // 先读一眼这个值,不够会直接打到 vApplicationMallocFailedHook 里死循环。
    xTaskCreate(finger_task, "finger", 256, NULL, 2, NULL);
}
