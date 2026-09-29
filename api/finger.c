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

    (void)arg;

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(FINGER_POLL_MS));

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
