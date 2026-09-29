#include "rc522_task.h"
#include "rfid.h"       // RC522_Init / PcdRequest / PcdAnticoll / PcdHalt / MI_OK
#include "AT24C04.h"    // 授权卡号的存储
#include "lock.h"       // lock_open / lock_is_busy / lock_lcd_set_title
#include "lcd.h"        // GREEN / RED / BLUE
#include "FreeRTOS.h"
#include "task.h"
#include "stdio.h"
#include "string.h"

// ══════════════════════════════════════════════════════════════════════════
//  授权卡号在 AT24C04A 里的存储布局(和 lock.c / wifi_cfg.c 顶上那两段并排看)
//
//      地址 0 ~ 7      锁密码      (lock.c)      标志 0xA5
//      地址 8 ~ 105    WiFi 凭据   (wifi_cfg.c)  标志 0xA6
//      地址 112 ~ 117  授权卡号    (本文件)      标志 0xA7   ← 就是这里
//
//  为什么从 112 开始,不接着 106 往后排:
//      AT24C04 的一页是 **16 字节**(页边界在 16 的整数倍上),而页写**不会进位**
//      —— 写到页尾会绕回本页开头把前面的数据盖掉。所以"标志 + 数据"这一整块
//      必须落在同一页里,才能用**一次** AT24C04_page_write() 写完,器件只做一次
//      内部写周期,**不会出现"标志已经有效、卡号还是乱码"的中间态**。
//      106 ~ 111 装不下 6 个字节还不跨页(106+6-1 = 111 其实刚好压线),
//      索性和 lock.c 一样取一个 16 的整数倍,112 ~ 117 稳稳落在 112~127 这一页里。
//
//  标志值 0xA7 故意和 0xA5(锁)、0xA6(WiFi)都不一样:三个模块共用一片 EEPROM,
//  万一以后谁把地址算错了读到别人的地盘,标志对不上至少能当场发现,
//  不会把 WiFi 名当成卡号去比。这是先例,后面再往这片 EEPROM 加东西照做。
// ══════════════════════════════════════════════════════════════════════════

#define RC522_EEPROM_ADDR   112     // 整块数据的起始地址(16 的倍数,一页写完成)
#define RC522_EEPROM_BYTES  6       // 标志1 + 长度1 + 卡号4
#define RC522_FLAG_VALUE    0xA7u
#define RC522_UID_LEN       4       // Mifare One 的 UID 是 4 字节

// ── 参数 ──
// 轮询周期为什么给到 200ms 这么"慢":
//   没卡的那一次寻卡,PcdComMF522() 内部会**空转着读寄存器等超时**
//   (兜底循环 1000 次,每次两个字节的 SPI 事务,实测量级 5ms 左右的忙等)。
//   RC522 没有"卡靠近了叫我"的中断脚接到 MCU 上(IRQ 没接),所以只能轮询,
//   周期定得太短就是白白烧 CPU,还会把低优先级的 wifi 任务一直往后推。
//   200ms 对"把卡贴上去"这个动作来说完全够灵敏。
#define RC522_POLL_MS      200  // 没卡时的轮询周期
#define RC522_RELEASE_MS   200  // 等卡拿开时的轮询周期
#define RC522_FAIL_MS      800  // "DENIED" 红字停留多久

// ── 管理员模式派下来的活(加卡 / 删卡)──
#define RC522_LEARN_TIMEOUT_MS  20000  // 333# 加卡:等卡放上来最多等多久
#define RC522_LEARN_POLL_MS     200    // 加卡等待期间的寻卡周期

//   为什么用标志而不是队列:一次只可能有一件活(管理员模式一次只派一个子模式)。
//   ★ 跨任务读写:admin(跑在 lock_task 里)写,本任务读。
//     读的时候要"取出并清零"一起做完,见 rc522_take_request()。
#define RC522_REQ_NONE   0
#define RC522_REQ_LEARN  1
#define RC522_REQ_ERASE  2

static volatile uint8_t s_learn_req;
static volatile uint8_t s_erase_req;

// 标题行提示语。**必须凑够 8 个字符**:标题行原本是 "PASSWORD"(8 个字符),
// 而 LCD_Font_Dis_Ascii() 只把每个字符格子里的像素写一遍,新文字短了就盖不掉
// 旧的尾巴,右边会留残影(见 lock.h 里 lock_lcd_set_title() 的说明)。
// "RFID" 后面补 4 个空格凑满。
#define RC522_TITLE_OK      "RFID    "
#define RC522_TITLE_NEW     "NEW CARD"
#define RC522_TITLE_DENY    "DENIED  "
#define RC522_TITLE_IDLE    "PASSWORD"

// ── 运行时状态 ──
static uint8_t s_uid[RC522_UID_LEN];    // 已授权的卡号,开机时从 EEPROM 载入 RAM
static uint8_t s_has_card;              // 0 = 还没录过卡,下一张刷上来的就是它

/**
 * 🟢 L2 工具代码 —— 纯打印,没有任何副作用;故意不返回信息给调用方
 * @brief  把卡号按十六进制打到串口
 * @param  tag  ASCII 标签(故意用英文:中文字符串字面量的编码在 armcc 上是个坑,
 *              见 armcc 的 GBK 解析问题,日志标签没必要去踩)
 * @param  uid  4 字节卡号
 * @return 无
 * @note   这张卡不是你想要的卡时,就是靠这行打印把卡号抄下来用的。
 */
static void rc522_print_uid(const char *tag, const uint8_t *uid)
{
    printf("RFID:%s %02X %02X %02X %02X\r\n",
           tag, uid[0], uid[1], uid[2], uid[3]);
}

/**
 * 🟡 L1 架构代码 —— 唯一一处写 EEPROM 的地方,"标志和卡号必须落在同一页、一次写完"
 * 是它和 lock.c 共同的正确性前提(否则掉电会留下半截数据);改地址前先读文件顶部的布局说明
 * @brief  把一张卡录成授权卡(写 EEPROM),同时更新 RAM 里的缓存
 * @param  uid 4 字节卡号
 * @return 无
 * @note   ⚠ **这是本模块唯一会阻塞的地方**:AT24C04_page_write() 末尾用
 *         Delay_Ms(AT24C04_TWR_MS) 忙等写周期 —— 那是 NOP 忙等,而且 delay.c
 *         是按 168MHz 标定的,84MHz 上实际要等 20ms 左右。
 *         这个代价可以接受,原因是**一辈子只发生一次**(首次录入那张卡),
 *         而且忙等期间中断照常响应,SysTick 不会丢,tick 也不会错。
 *         真要做成"随时可以加卡/删卡",就得把这次写改成状态机 + vTaskDelay,
 *         那是另一个工程量,现在不做。
 *         写失败时**不会**把 s_has_card 置起来 —— 免得 RAM 里认了这张卡、
 *         掉电之后又没了,让人以为"明明录过了却打不开"。
 */
static void rc522_enroll(const uint8_t *uid)
{
    uint8_t buf[RC522_EEPROM_BYTES];

    buf[0] = RC522_FLAG_VALUE;
    buf[1] = RC522_UID_LEN;
    memcpy(&buf[2], uid, RC522_UID_LEN);

    if (AT24C04_page_write(RC522_EEPROM_ADDR, buf, RC522_EEPROM_BYTES) != 0)
    {
        printf("RFID:卡号写入 EEPROM 失败,本次只当临时卡用,掉电就没了\r\n");
        return;
    }

    memcpy(s_uid, uid, RC522_UID_LEN);
    s_has_card = 1;
}

/**
 * 🟡 L1 架构代码 —— 等卡那一段的计时必须用无符号差值(xTaskGetTickCount 会 49.7 天回绕),超时判断写错的现象是这个任务永远卡在加卡模式里不再刷卡
 * @brief  加卡模式:等一张卡放上来,把它录成授权卡(管理员模式 333# 派下来的活)
 * @note   流程和主循环里的常规寻卡一样(寻卡 → 防冲突 → PcdHalt → 存),区别只有两点:
 *           1) 这里是**有超时的等待**(20 秒),常规寻卡是"这一轮没卡就下一轮";
 *           2) 认卡那一步直接覆盖,不做比对 —— 用户说了要加卡,那就加。
 * @note   录完还要等卡拿开:不然手一慢,主循环下一轮就会把同一张卡当成一次
 *         常规刷卡,顺手把锁开了。
 * @note   屏幕抢不到就不显示进度,活照干(见 lock.h 的 lock_screen_take)。
 *         111# 会同时给指纹和刷卡派活,两个都画就会互相盖。
 * @note   RC522 的 SPI 事务是独占的 —— 这个函数跑起来的时候主循环里那套
 *         寻卡逻辑是停着的(同一个任务,顺序执行),所以不会有并发访问。
 */
static void rc522_learn(void)
{
    uint8_t tag_type[2];
    uint8_t uid[RC522_UID_LEN];
    uint32_t start = xTaskGetTickCount();
    uint8_t got_ui;

    got_ui = lock_screen_take(LOCK_SCREEN_RFID);

    if (got_ui)
    {
        lock_lcd_big_line(0, "ADD CARD");
        lock_lcd_big_line(1, "");
        lock_lcd_big_line(2, "PLACE CARD");
        lock_lcd_big_line(3, "");
        lock_lcd_big_line(4, "");
        lock_lcd_big_line(5, "");
    }

    /* ── 1) 等卡放上来 ── */
    for (;;)
    {
        if (PcdRequest(PICC_REQALL, tag_type) == MI_OK)
        {
            break;
        }

        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(RC522_LEARN_TIMEOUT_MS))
        {
            printf("RFID:加卡超时,%d 秒没等到卡\r\n", RC522_LEARN_TIMEOUT_MS / 1000);
            if (got_ui)
            {
                lock_lcd_big_line(2, "TIMEOUT");
                vTaskDelay(pdMS_TO_TICKS(RC522_FAIL_MS));
                lock_screen_give(LOCK_SCREEN_RFID);
            }
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(RC522_LEARN_POLL_MS));
    }

    /* ── 2) 防冲突,拿到 4 字节 UID ── */
    if (PcdAnticoll(uid) != MI_OK)
    {
        printf("RFID:加卡时防冲突失败,重试一次\r\n");
        if (got_ui)
        {
            lock_lcd_big_line(2, "READ FAILED");
            vTaskDelay(pdMS_TO_TICKS(RC522_FAIL_MS));
            lock_screen_give(LOCK_SCREEN_RFID);
        }
        return;
    }

    PcdHalt(); // 让卡休眠,免得它每 200ms 就响应一次寻卡
    rc522_print_uid("LEARN", uid);

    /* ── 3) 存起来。和首次自动录入走的是同一个 rc522_enroll() ── */
    rc522_enroll(uid);
    rc522_print_uid("SAVED", uid);

    if (got_ui)
    {
        lock_lcd_big_line(2, "CARD SAVED");
    }

    /* ── 4) 等卡拿开 ── */
    while (PcdRequest(PICC_REQALL, tag_type) == MI_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(RC522_RELEASE_MS));
    }

    if (got_ui)
    {
        vTaskDelay(pdMS_TO_TICKS(RC522_FAIL_MS));
        lock_screen_give(LOCK_SCREEN_RFID);
    }
}

/**
 * 🔴 L0 生死线代码 —— 删完之后这门锁就回到"第一张刷上来的卡自动成为钥匙"的状态,而且屏幕上只提示一句 CARD DELETED,看不出这层后果
 * @brief  删除已授权的卡号(管理员模式 111# 派下来的活)
 * @note   实现上是把 EEPROM 里那 6 个字节整体清零(标志字节写 0 = 没有授权卡),
 *         一次页写完成 —— 仍然是"标志和数据同一个写周期落盘",不会出现
 *         "标志已失效、卡号还在"的中间态。
 * @note   ⚠ **删完之后 s_has_card 归零,主循环会退回"第一张刷上来的卡即录入"**。
 *         这是 111# 想要的效果(回到出厂状态),但要知道它同时意味着:
 *         谁在主人之前刷第一张卡,谁就成了新的授权卡。见 rc522_task.h 的说明。
 */
static void rc522_erase(void)
{
    uint8_t buf[RC522_EEPROM_BYTES];
    uint8_t got_ui;

    got_ui = lock_screen_take(LOCK_SCREEN_RFID);

    if (got_ui)
    {
        lock_lcd_big_line(0, "CLEAR ALL");
        lock_lcd_big_line(1, "");
        lock_lcd_big_line(2, "DELETING CARD");
        lock_lcd_big_line(3, "");
        lock_lcd_big_line(4, "");
        lock_lcd_big_line(5, "");
    }

    memset(buf, 0, sizeof(buf)); // 标志 0 = 没有授权卡

    // ⚠ 这里有一次约 20ms 的 EEPROM 忙等(和 rc522_enroll 同一个代价,见它的说明)
    if (AT24C04_page_write(RC522_EEPROM_ADDR, buf, RC522_EEPROM_BYTES) != 0)
    {
        printf("RFID:删除授权卡号失败(EEPROM 没应答),原卡号仍有效\r\n");
        if (got_ui)
        {
            lock_lcd_big_line(2, "DELETE FAILED");
        }
    }
    else
    {
        // 写盘成功了才动 RAM —— 反过来的话会出现"这次不认卡了、重启之后又认了"
        memset(s_uid, 0, sizeof(s_uid));
        s_has_card = 0;

        printf("RFID:授权卡号已删除 —— 下一张刷上来的卡会被自动录成新的授权卡\r\n");
        if (got_ui)
        {
            lock_lcd_big_line(2, "CARD DELETED");
        }
    }

    if (got_ui)
    {
        vTaskDelay(pdMS_TO_TICKS(RC522_FAIL_MS));
        lock_screen_give(LOCK_SCREEN_RFID);
    }
}

/**
 * 🟡 L1 架构代码 —— "检查 + 清零"必须在同一个临界区里:取出来之后、清零之前对方又置了一次,这次请求就被这次清零吃掉;而这条原子性是"派活不丢"的全部保证
 * @brief  取出并清掉一个待办的加卡/删卡请求
 * @return RC522_REQ_NONE / RC522_REQ_LEARN / RC522_REQ_ERASE
 */
static uint8_t rc522_take_request(void)
{
    uint8_t req = RC522_REQ_NONE;

    taskENTER_CRITICAL();
    if (s_learn_req)
    {
        s_learn_req = 0;
        req = RC522_REQ_LEARN;
    }
    else if (s_erase_req)
    {
        s_erase_req = 0;
        req = RC522_REQ_ERASE;
    }
    taskEXIT_CRITICAL();

    return req;
}

// 🟢 L2 工具代码 —— 纯置位,没有副作用也没有先后要求;"投递意图而非执行"这条分工由调用方遵守
void rc522_request_learn(void)
{
    s_learn_req = 1;
}

// 🔴 L0 生死线代码 —— 它本身不删任何东西,但触发的那个状态(没有授权卡)正是"下一张卡自动成为钥匙"的入口,删之前想清楚
void rc522_request_erase(void)
{
    s_erase_req = 1;
}

/**
 * 🟡 L1 架构代码 —— 业务主流程 + 与密码/指纹共用 LCD 的互斥策略(lock_is_busy 预检):
 * "刷卡 → 比对 → 调用 lock_open → 等卡拿开"这几步的顺序和延时是它在正确性上的全部内容,
 * 少一步"等卡拿开"同一张卡就会被反复刷
 * @brief  刷卡开锁任务
 * @note   平时 200ms 睡一次;没卡时那一次寻卡本身要 ~5ms 忙等(见 RC522_POLL_MS
 *         的说明),所以轮询周期不能定得太短。
 * @note   刷卡、密码、指纹三条路都会写 LCD。LCD 底层虽然已经逐像素上锁
 *         (见 lcd.c),这里仍然用 lock_is_busy() 把整串字避开正在进行的开锁流程,
 *         免得刚画出 "RFID" 就被开锁标题盖掉。真正的仲裁仍然是 lock_open() 自己。
 * @note   流程里**没有** PcdSelect / PcdAuthState:
 *         只认卡号的话,寻卡 + 防冲突拿到 UID 就够了 —— 再往下走"选卡 + 验证扇区密钥"
 *         是为了读写卡里的数据块,本功能用不上。以后要在卡里存东西再说。
 */
static void rc522_task(void *arg)
{
    uint8_t tag_type[2];
    uint8_t uid[RC522_UID_LEN];
    uint8_t req;

    (void)arg;

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(RC522_POLL_MS));

        // ── 管理员模式派下来的活优先 ──
        //    ⚠ 必须排在下面 lock_is_busy() 那道预检**之前**:加卡和删卡本来就是
        //      "用密码开完锁之后"发生的,那时候锁多半还在开锁/保持/回锁的过程里,
        //      排在后面会被那个 continue 一直挡在门外,直到回锁完才轮得到。
        req = rc522_take_request();
        if (req == RC522_REQ_LEARN)
        {
            rc522_learn();
            continue;
        }
        if (req == RC522_REQ_ERASE)
        {
            rc522_erase();
            continue;
        }

        // 正在开锁/回锁:这轮不刷 —— 刷了也会被 lock_open() 拒掉,还会去抢 LCD
        if (lock_is_busy())
        {
            continue;
        }

        // ── 1) 寻卡。没有卡就直接下一轮 ──
        //    用 PICC_REQALL(0x52,WUPA)而不是 PICC_REQIDL(0x26,REQA):
        //    WUPA 能把**已经进了 HALT 状态的卡**重新唤醒。下面流程结束时会
        //    PcdHalt() 让卡休眠,靠的就是这个才能再把它叫醒。
        if (PcdRequest(PICC_REQALL, tag_type) != MI_OK)
        {
            continue;
        }

        // ── 2) 防冲突,拿到 4 字节 UID ──
        //    多张卡同时在感应区时它会靠 BCC 校验挑出一张;单张卡也必须走这一步,
        //    因为寻卡那步只回了 2 字节卡类型,不含卡号。
        if (PcdAnticoll(uid) != MI_OK)
        {
            continue;
        }

        // 让卡休眠。不做的话,同一张卡会一直响应寻卡命令,
        // 每 200ms 就"刷"一次,锁会被反复触发。
        PcdHalt();

        rc522_print_uid("READ", uid);

        // ── 3) 认卡 ──
        if (!s_has_card)
        {
            // 还没录过卡:这张就是第一张,录进去并直接开锁
            rc522_enroll(uid);
            rc522_print_uid("ENROLL", uid);
            lock_open((uint8_t *)RC522_TITLE_NEW, GREEN);
        }
        else if (memcmp(uid, s_uid, RC522_UID_LEN) == 0)
        {
            // 这一行打印是补上的:原来这里闷头开锁,一句日志都没有 ——
            // 上板时看到的是"READ 76 B3 0B 25"之后锁就开了、中间什么都不打,
            // 会让人怀疑是不是"随便什么卡都能开"。白名单没生效的样子和它一模一样。
            rc522_print_uid("MATCH", uid);

            // 走公共开锁流程。它内部还会再判一次忙,这里不用自己防
            lock_open((uint8_t *)RC522_TITLE_OK, GREEN);
        }
        else
        {
            rc522_print_uid("DENIED", uid);

            // 比对这会儿对方可能已经开始开锁了,那就别去盖它的界面
            if (!lock_is_busy())
            {
                lock_lcd_set_title((uint8_t *)RC522_TITLE_DENY, RED);
                vTaskDelay(pdMS_TO_TICKS(RC522_FAIL_MS));
                lock_lcd_set_title((uint8_t *)RC522_TITLE_IDLE, BLUE);
            }
        }

        // ── 4) 等卡拿开 ──
        //    为什么非要等:上面已经 PcdHalt() 过了,但 WUPA 能把它叫醒,
        //    所以"还读得到"就等于"卡还贴在感应区上"。不等的话,手一慢
        //    就会连着开好几次锁。
        //    (finger.c 里等着手指松开是同一个道理,只不过那边有 DETECT 脚可用)
        while (PcdRequest(PICC_REQALL, tag_type) == MI_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(RC522_RELEASE_MS));
        }
    }
}

/**
 * 🟡 L1 架构代码 —— 生命周期约束和 lock_init/finger 一致:必须赶在调度器启动前调用,内含 EEPROM 读是忙等
 * @brief  刷卡模块初始化:配好 RC522,再从 EEPROM 载入已授权的卡号
 * @return 无
 * @note   依赖 i2c_bus_config(&i2c_bus_at24c04) 已经配好 I2C 总线,
 *         所以 main.c 里必须排在它后面。
 */
void rc522_init(void)
{
    uint8_t buf[RC522_EEPROM_BYTES];

    RC522_Init(); // SPI3 + 片选/复位 GPIO + 复位芯片 + 开天线

    s_has_card = 0;
    memset(s_uid, 0, sizeof(s_uid));

    // 读失败(总线没应答)和"没录过卡"是两回事,但对本模块的处理是一样的:
    // 都当成"还没有授权卡",让第一张刷上来的卡去录入。
    if (AT24C04_read(RC522_EEPROM_ADDR, buf, RC522_EEPROM_BYTES) == 0 && buf[0] == RC522_FLAG_VALUE && buf[1] == RC522_UID_LEN)
    {
        memcpy(s_uid, &buf[2], RC522_UID_LEN);
        s_has_card = 1;
        rc522_print_uid("AUTHORIZED", s_uid);
    }

    if (!s_has_card)
    {
        printf("RFID:还没有授权卡 —— 刷第一张卡即录入,并开锁一次\r\n");
    }
}

// 🟡 L1 架构代码 —— 栈 256 字是"任务里要跑 printf"定的(和 finger 同量级);12KB 堆加完它只剩约 1.9KB,再添任务前先看下面打印的余量
void rc522_tasks_init(void)
{
    // 栈 256 字(1KB):任务里会调到 printf,还要跑 RC522 的收发函数
    // (PcdComMF522 里有 18 字节的栈上缓冲区),和 finger_task 同量级。
    xTaskCreate(rc522_task, "rc522", 256, NULL, 2, NULL);

    // ── 堆余量实测 ──
    // 为什么要在这儿打一行:12KB 的 heap_4 已经切出去 8 个任务了,这个模块
    // 又加了第 9 个。**堆不够不会报错,会直接跳进 vApplicationMallocFailedHook
    // 里的 for(;;) 死循环** —— 上板看到的是"什么都不动、串口也没输出",
    // 极难查。打一行数字出来,一眼就知道还剩多少。
    //
    // 这笔账(估算,不是实测):
    //   任务栈  cy9cm_scan 256 + finger 256 + key_scan 256 + rc522 256
    //         + key_action 128 + lock 320 + wifi 512 + idle 128 + timer 128
    //         = 2240 字 = 8960 字节
    //   再加 9 个 TCB(每个约 110 字节)和 3 个队列 ≈ 1.2KB
    //   合计约 10.2KB,12KB 的堆**只剩 1.8~1.9KB**。
    // 所以:再加任务之前先看这行打印,别贴着上限走。
    printf("FreeRTOS:建完 rc522 任务后堆余量 %u 字节\r\n",
           (unsigned)xPortGetFreeHeapSize());
}
