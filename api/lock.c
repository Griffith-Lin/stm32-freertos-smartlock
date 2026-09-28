#include "lock.h"
#include "CY9CM.h"      // cy9cm_key_queue:触摸板按键事件
#include "AT24C04.h"    // 密码存储
#include "L9110S.h"     // 开锁电机
#include "lcd.h"        // 输入界面
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "stdio.h"

// ══════════════════════════════════════════════════════════════════════════
//  密码在 AT24C04A 里的存储布局(512 字节的 EEPROM,这里只用了前 8 个)
//
//      地址 0        初始化标志 = 0xA5      "这块芯片里的密码已经写过了"
//      地址 1        密码长度   = 6
//      地址 2 ~ 7    密码本体,存 ASCII 码
//
//  为什么存 ASCII 而不是 BCD:触摸板返回的就是 '0'~'9' 字符,存 ASCII 可以
//  直接比较,不用来回转换;用编程器读出来一眼就是明文,调试方便。
//
//  为什么只用一个标志就够了:这 8 个字节都在 EEPROM 第 0 页(页大小 16 字节)
//  之内,一次 AT24C04_page_write() 就写完了,器件只做一次内部写周期。
//  标志和密码在同一个写周期里落盘,**不会出现"标志有效但密码是乱码"的中间态**。
//  (如果数据跨页分多次写,就必须把"有效标志"安排成最后一个写,否则中途掉电
//   会留下半截数据;更专业的做法是再加一字节 CRC 校验整个块。)
// ══════════════════════════════════════════════════════════════════════════

#define LOCK_EEPROM_ADDR    0                       // 整块数据的起始地址
#define LOCK_EEPROM_BYTES   (2 + LOCK_PWD_LEN)      // 标志1 + 长度1 + 密码6 = 8
#define LOCK_FLAG_VALUE     0xA5u

// 出厂默认密码:666666(首次上电时写进 EEPROM)
static const uint8_t LOCK_DEFAULT_PWD[LOCK_PWD_LEN] = {
    '6', '6', '6', '6', '6', '6'
};

// ── LCD 版面(屏是 240 宽 × 320 高,左上角为原点)──
#define LOCK_LCD_TITLE_ROW  80
#define LOCK_LCD_TITLE_COL  72      // 8 个字 × (24/2) = 96 像素,居中
#define LOCK_LCD_INPUT_ROW  160
#define LOCK_LCD_INPUT_COL  72      // 6 个字符 × (32/2) = 96 像素,居中
#define LOCK_LCD_TITLE_SIZE 32
#define LOCK_LCD_INPUT_SIZE 32

// 密码错误的提示语。**必须凑够 8 个字符**:标题行原本是 "PASSWORD"(8 字符),
// 而 LCD_Font_Dis_Ascii() 只把每个字符格子里的像素写一遍,新文字短了盖不掉
// 旧的尾巴,右边会留下 "ORD" 残影。所以 "ERROR" 后面补 3 个空格凑满 8 个。
// (finger.c 里的 "FINGER  " 是同一个道理)
#define LOCK_LCD_ERROR_TEXT "ERROR   "
#define LOCK_LCD_ERROR_MS   800     // 红字停留多久(ms)

// ── 运行时状态 ──
static uint8_t s_pwd[LOCK_PWD_LEN];     // 正确的密码,开机时从 EEPROM 载入 RAM
static uint8_t s_input[LOCK_PWD_LEN];   // 用户已经输入的内容
static uint8_t s_input_len;             // 已输入几位
static uint8_t s_unlocking;             // 1 = 正在开锁/回锁,这期间不响应按键

// ── 锁控命令队列(远程来源:MQTT 云端下发)──
//    投递方是 wifi_task,消费方是 lock_task。见 lock.h 里 lock_cmd_t 的说明。
QueueHandle_t lock_cmd_queue = NULL;

// ── 锁的状态机 ──
//    原来"开锁 → 保持 3 秒 → 回锁"是一整块写死在 lock_open() 里的阻塞流程。
//    好处是直白;坏处是**这 4 秒里 lock_task 完全没法响应任何东西** ——
//    远程关锁命令只能在队列里干等,等轮到自己时锁早自动回锁了,于是"关锁"
//    这个动作等于没实现。
//    拆成状态机之后,lock_task 每 LOCK_TICK_MS 醒一次推进一步,
//    保持期随时能被关锁命令打断。
//
//        开锁 ┌──────────┐ 行程走完  ┌────────┐ 3 秒到 或 收到关锁 ┌──────────┐ 行程走完
//      ───────►│ OPENING  │──────────►│  OPEN  │──────────────────►│ CLOSING  │──────────► IDLE
//             └──────────┘           └────────┘                   └──────────┘
typedef enum
{
    LOCK_IDLE = 0,      // 锁着,静止
    LOCK_OPENING,       // 电机正在推出锁舌
    LOCK_OPEN,          // 锁舌已推出,保持中(等自动回锁,或被关锁命令打断)
    LOCK_CLOSING        // 电机正在收回锁舌
} lock_state_t;

static lock_state_t s_state = LOCK_IDLE;
static uint32_t     s_phase_start;      // 进入当前状态的时刻(FreeRTOS tick)
static uint8_t      s_close_req;        // 1 = 有人要求立刻回锁
                                        //     **只在 lock_task 里读写**(由
                                        //     lock_handle_remote 置位、lock_tick 消费),
                                        //     所以天然不需要临界区。

// 状态机的推进节拍。20ms 的来历:
//   电机行程是 500ms 级的,20ms 的判定误差(最快提前 20ms 刹车)肉眼看不出来;
//   而关锁命令最坏要等 20ms 才被响应,也感觉不到。
//   再小就没意义了 —— 只是让空闲的 lock_task 多醒几次。
#define LOCK_TICK_MS    20


/**
 * @brief  在 LCD 上画一行 ASCII 文字
 * @param  row/col  左上角坐标
 * @param  size     字号,只支持 16 / 24 / 32
 * @param  fg/bg    前景色 / 背景色
 * @param  text     以 '\0' 结尾的 ASCII 串
 * @note   不需要先 LCD_Clear:LCD_Font_Dis_Ascii() 会把每个字符格子的
 *         **每一个像素**都写一遍(笔画写前景色,其余写背景色),相当于自带清底。
 *         但这只在"新文字不比旧文字短"时成立,否则旧内容的尾巴会留下来。
 * @note   传进来的必须是 ASCII(字节 < 0xA1)。**不能传中文的 UTF-8 字节**
 *         —— LCD_Font_Dis 会把 >= 0xA1 的字节当成 GBK 双字节汉字去 W25Q 字库里
 *         查表,而字库存的是 GBK 点阵,拿 UTF-8 去查只会画出乱码。
 *         想在屏上显示中文,得用 GBK 字节(见文件末尾说明)。
 */
static void lock_lcd_text(uint16_t row, uint16_t col, uint8_t size,
                          uint16_t fg, uint16_t bg, uint8_t *text)
{
    LCD_Font_Dis(row, col, size, fg, bg, text);
}

/**
 * @brief  重画下面那行密码掩码:已输入的显示 '*' ,还没输入的显示 '-'
 */
static void lock_lcd_input(void)
{
    uint8_t mask[LOCK_PWD_LEN + 1];
    uint8_t i;

    for (i = 0; i < LOCK_PWD_LEN; i++)
    {
        mask[i] = (i < s_input_len) ? '*' : '-';
    }
    mask[LOCK_PWD_LEN] = '\0';

    lock_lcd_text(LOCK_LCD_INPUT_ROW, LOCK_LCD_INPUT_COL, LOCK_LCD_INPUT_SIZE,
                  BLACK, WHITE, mask);
}

/**
 * @brief  清空已输入的内容并刷新界面
 */
static void lock_clear_input(void)
{
    s_input_len = 0;
    lock_lcd_input();
}

/**
 * @brief  在标题行闪一下红色的 ERROR,再恢复蓝色的 PASSWORD
 * @note   必须在任务上下文调用(内部用 vTaskDelay 让出 CPU)。
 *         这 800ms 里 lock_task 自己就停在本函数里,期间按的键会堆在
 *         cy9cm_key_queue 里(队列深度 8),等回来后按顺序继续处理。
 * @note   为什么不用 Delay_Ms():那是 NOP 忙等,会把 CPU 占死 800ms,
 *         同优先级和低优先级的任务全部饿死 —— 和 lock_open() 里不用
 *         L9110S_move() 是同一个原因。
 */
static void lock_lcd_show_error(void)
{
    lock_lcd_text(LOCK_LCD_TITLE_ROW, LOCK_LCD_TITLE_COL, LOCK_LCD_TITLE_SIZE,
                  RED, WHITE, (uint8_t *)LOCK_LCD_ERROR_TEXT);
    vTaskDelay(pdMS_TO_TICKS(LOCK_LCD_ERROR_MS));
    lock_lcd_text(LOCK_LCD_TITLE_ROW, LOCK_LCD_TITLE_COL, LOCK_LCD_TITLE_SIZE,
                  BLUE, WHITE, (uint8_t *)"PASSWORD");
}


/**
 * @brief  从 EEPROM 加载密码;如果是第一次上电(没有有效标志),写入默认密码
 * @note   只在裸机阶段调用一次。加载之后密码缓存进 s_pwd[],任务阶段不再访问
 *         EEPROM —— 这样既避开了 I2C 总线的并发问题,也不用在任务里等 tWR。
 *         代价是"改了密码要重新上电才生效",对门锁来说完全可以接受。
 */
void lock_init(void)
{
    uint8_t buf[LOCK_EEPROM_BYTES];
    uint8_t i;
    uint8_t need_default = 0;

    // ── 1. 读出标志 + 长度 + 密码 ──
    if (AT24C04_read(LOCK_EEPROM_ADDR, buf, LOCK_EEPROM_BYTES) != 0)
    {
        printf("EEPROM 读取失败,本次先用默认密码\r\n");
        need_default = 1;
    }
    else if ((buf[0] != LOCK_FLAG_VALUE) || (buf[1] != LOCK_PWD_LEN))
    {
        printf("EEPROM 里还没有有效密码,写入默认值\r\n");
        need_default = 1;
    }
    else
    {
        printf("已从 EEPROM 载入密码\r\n");
    }

    // ── 2. 第一次上电:把默认密码写进去 ──
    if (need_default)
    {
        buf[0] = LOCK_FLAG_VALUE;
        buf[1] = LOCK_PWD_LEN;
        for (i = 0; i < LOCK_PWD_LEN; i++)
        {
            buf[2 + i] = LOCK_DEFAULT_PWD[i];
        }

        if (AT24C04_page_write(LOCK_EEPROM_ADDR, buf, LOCK_EEPROM_BYTES) != 0)
        {
            printf("默认密码写入 EEPROM 失败,请检查 AT24C04 接线\r\n");
        }

        for (i = 0; i < LOCK_PWD_LEN; i++)
        {
            s_pwd[i] = LOCK_DEFAULT_PWD[i];
        }
    }
    else
    {
        for (i = 0; i < LOCK_PWD_LEN; i++)
        {
            s_pwd[i] = buf[2 + i];
        }
    }

    s_input_len = 0;
    s_unlocking = 0;

    // ── 3. 复位后就进输入密码界面 ──
    LCD_Clear(0, 320, 0, 240, WHITE);
    lock_lcd_text(LOCK_LCD_TITLE_ROW, LOCK_LCD_TITLE_COL, LOCK_LCD_TITLE_SIZE,
                  BLUE, WHITE, (uint8_t *)"PASSWORD");
    lock_lcd_input();

    printf("密码锁就绪:%d 位密码,输入 '*' 清空, '#' 确认\r\n", LOCK_PWD_LEN);
}


/**
 * @brief  收下一个数字键:追加到输入缓冲区
 * @note   已经输满 6 位之后再多按的数字直接丢掉 —— 必须按 '#' 确认
 *         或按 '*' 清空重来,避免"多按一下就把密码顶掉"这种别扭行为。
 */
static void lock_input_digit(uint8_t digit)
{
    if (s_input_len >= LOCK_PWD_LEN)
    {
        return;
    }

    s_input[s_input_len] = digit;
    s_input_len++;

    printf("已输入 %d 位\r\n", s_input_len);
    lock_lcd_input();
}


void lock_lcd_set_title(uint8_t *title, uint16_t color)
{
    lock_lcd_text(LOCK_LCD_TITLE_ROW, LOCK_LCD_TITLE_COL, LOCK_LCD_TITLE_SIZE,
                  color, WHITE, title);
}

uint8_t lock_is_busy(void)
{
    return s_unlocking;
}

/**
 * @brief  进入"推锁舌"状态:开机界面 + 电机正转
 * @param  title 开锁期间标题行显示的文字
 * @param  color 标题颜色
 * @note   只**起头**,不等待。行程什么时候走完由 lock_tick() 判断。
 */
static void lock_begin_opening(uint8_t *title, uint16_t color)
{
    // ⚠ 先推状态机,再动 LCD 和电机。
    //   因为 lock_open() 是在临界区里把 s_unlocking 置 1 的,出了临界区那
    //   一刻 s_state 还是 IDLE。中间要是先花几毫秒刷 LCD(8 个 32 号字,
    //   SPI 慢慢写),就会出现"s_unlocking=1 但 s_state=IDLE"这种自相矛盾的
    //   瞬间 —— 调试时读到能白查半天(实测确实踩到过)。先写状态就没这窗口。
    s_state       = LOCK_OPENING;
    s_phase_start = xTaskGetTickCount();
    s_close_req   = 0;      // 上一次的关锁请求不能留到这一次,否则一开就关

    lock_lcd_set_title(title, color);
    lock_clear_input();

    // 开锁行程:正转推出去。停不停由 lock_tick() 在 500ms 后决定 ——
    // 万一行程中途收到关锁命令,那边刹车后会直接转 CLOSING。
    // (上面刷 LCD 花掉的几毫秒会算进行程里,500ms 的尺度上无所谓)
    L9110S_control(MOTOR_FORWARD);

    printf("开锁中,保持 %d 秒后自动回锁(期间收到关锁命令会立刻回锁)\r\n",
           LOCK_OPEN_MS / 1000);
}

/**
 * @brief  进入"收锁舌"状态:电机反转
 * @note   自动回锁和被远程关锁打断,走的都是这一个出口。
 */
static void lock_begin_closing(void)
{
    // 和 lock_begin_opening() 一样,先写状态再动电机,保持
    // "s_state 和实际动作一致"这条不变量。
    s_state       = LOCK_CLOSING;
    s_phase_start = xTaskGetTickCount();
    s_close_req   = 0;

    L9110S_control(MOTOR_REVERSE);

    printf("回锁中...\r\n");
}

/**
 * @brief  推进锁的状态机 —— 由 lock_task 每 LOCK_TICK_MS 调一次
 * @note   电机只在三个"动作中"的状态里被操作,而推进又只发生在 lock_task
 *         这一个任务里,所以 L9110S 的 FORWARD/REVERSE/BRAKE 天然是串行的,
 *         不需要额外加锁。lock_open() 那边靠 s_unlocking 保证"IDLE 才准进",
 *         进来之后 s_state 立刻变 OPENING,两次开锁也就撞不到一起。
 * @note   计时用 xTaskGetTickCount() 而不是 GetTim10Tick():本来就在任务里,
 *         用调度器的 tick 最自然,而且和 vTaskDelay 是同一个时基,少一层换算。
 *         (tick 是 32 位的,`now - start` 这种**无符号差值**写法能正确跨过
 *          49.7 天的回绕,不用自己处理。)
 */
static void lock_tick(void)
{
    uint32_t now = xTaskGetTickCount();

    switch (s_state)
    {
        // ── 正在推锁舌:等行程走完,然后刹车 ──
        case LOCK_OPENING:
            if ((now - s_phase_start) >= LOCK_MOTOR_MS)
            {
                L9110S_control(MOTOR_BRAKE);

                if (s_close_req)
                {
                    // 锁舌刚推到位就收到关锁 —— 直接收回去,不要傻等那 3 秒。
                    // (这是"立即回锁"在 OPENING 阶段的落点)
                    printf("开锁过程中收到关锁命令,立即回锁\r\n");
                    lock_begin_closing();
                }
                else
                {
                    s_state       = LOCK_OPEN;
                    s_phase_start = now;
                }
            }
            break;

        // ── 保持中:到点自动回锁,或被关锁命令提前打断 ──
        case LOCK_OPEN:
            if (s_close_req)
            {
                printf("收到关锁命令,立即回锁\r\n");
                lock_begin_closing();
            }
            else if ((now - s_phase_start) >= LOCK_OPEN_MS)
            {
                lock_begin_closing();
            }
            break;

        // ── 正在收锁舌:等行程走完,收工 ──
        case LOCK_CLOSING:
            if ((now - s_phase_start) >= LOCK_MOTOR_MS)
            {
                L9110S_control(MOTOR_BRAKE);
                s_state = LOCK_IDLE;

                lock_lcd_set_title((uint8_t *)"PASSWORD", BLUE);
                lock_clear_input();

                printf("已回锁\r\n");

                // 到这一刻才放开:整个开→保持→关的期间,按键和远程命令
                // 都会被拒(s_unlocking = 1)。
                s_unlocking = 0;
            }
            break;

        // ── LOCK_IDLE:静止,没什么可推进的 ──
        default:
            break;
    }
}

/**
 * @brief  开锁 —— 本地开锁来源的公共出口(密码、指纹都走这里)
 * @param  title 开锁期间标题行显示的 ASCII 文字
 * @param  color 标题颜色
 * @return 1 = 已受理;0 = 已有开锁流程在进行,本次被忽略
 * @note   **非阻塞**:只抢标志、写 LCD、把状态机推到 OPENING 就返回,
 *         整个动作耗时不到 1ms。后面 500ms 行程 + 3 秒保持 + 500ms 回锁,
 *         全部由 lock_task 的 lock_tick() 推进。
 * @note   为什么要拆:原来这里一气睡 4 秒,期间 lock_task 根本回不来收
 *         lock_cmd_queue,远程"关锁"只能在队列里干等到流程自己走完 ——
 *         那这个命令就等于没实现。拆开之后保持期随时能被打断。
 * @note   **不能用 L9110S_move()** ——它内部是
 *         `while (GetTim10Tick() - start < ms);` 的忙等,会把 CPU 整整占死
 *         500ms,同优先级和低优先级的任务全部饿死。这和 AT24C04 里
 *         Delay_Ms 要换成 vTaskDelay 是同一个道理。
 * @note   **不能在中断里调**:要写 LCD、还要进出临界区,都不是中断安全的。
 *         中断里想开锁就 post 一条 LOCK_CMD_OPEN 到 lock_cmd_queue。
 */
uint8_t lock_open(uint8_t *title, uint16_t color)
{
    /* 检查 + 占用标志一起放进临界区:如果分开写,中间被 SysTick 切走一次,
       另一个开锁来源(比如指纹任务)就会读到"没人开锁",然后两边一起驱动电机 */
    taskENTER_CRITICAL();
    if (s_unlocking)
    {
        taskEXIT_CRITICAL();
        printf("开锁/回锁进行中,本次请求忽略\r\n");
        return 0;
    }
    s_unlocking = 1;
    taskEXIT_CRITICAL();

    lock_begin_opening(title, color);
    return 1;
}


/**
 * @brief  按下 '#' :校验输入的密码
 * @note   位数不对也算错误,一并清空重输并显示红色 ERROR。
 *         先清输入行再弹错误提示 —— 反过来的话,ERROR 停留的 800ms 里
 *         下面那行的 '******' 还挂着,看着像是"密码还在,但已经错了"。
 */
static void lock_verify(void)
{
    uint8_t i;

    if (s_input_len != LOCK_PWD_LEN)
    {
        printf("密码错误:只输了 %d 位,需要 %d 位\r\n", s_input_len, LOCK_PWD_LEN);
        lock_clear_input();
        lock_lcd_show_error();
        return;
    }

    for (i = 0; i < LOCK_PWD_LEN; i++)
    {
        if (s_input[i] != s_pwd[i])
        {
            printf("密码错误\r\n");
            lock_clear_input();
            lock_lcd_show_error();
            return;
        }
    }

    printf("密码正确\r\n");
    lock_open((uint8_t *)"UNLOCKED", GREEN);
}


/**
 * @brief  处理一个触摸板按键
 * @param  key '0'~'9' / '*' / '#'
 * @note   开锁/回锁过程中一律忽略 —— 那时 LCD 上显示的是 "UNLOCKED",
 *         再让按键去改输入行只会把界面搞乱。
 */
static void lock_handle_key(uint8_t key)
{
    if (s_unlocking)
    {
        return;
    }

    if ((key >= '0') && (key <= '9'))
    {
        lock_input_digit(key);
    }
    else if (key == '*')
    {
        printf("清空重输\r\n");
        lock_clear_input();
    }
    else if (key == '#')
    {
        lock_verify();
    }
}

/**
 * @brief  处理一条远程锁控命令(MQTT 云端下发)
 * @param  cmd LOCK_CMD_OPEN / LOCK_CMD_CLOSE
 * @note   **只在本任务里跑**,所以 s_close_req 不需要临界区保护。
 */
static void lock_handle_remote(lock_cmd_t cmd)
{
    switch (cmd)
    {
        case LOCK_CMD_OPEN:
            printf("远程开锁请求\r\n");
            // 不直接驱动电机,走公共出口 —— 忙的话会被拒,并且打印原因。
            // 这样"同一时刻只有一个开锁流程"的互斥在三条路(密码/指纹/云端)上
            // 是同一份代码,不会各写一遍各漏一处。
            lock_open((uint8_t *)"REMOTE  ", GREEN);
            break;

        case LOCK_CMD_CLOSE:
            if (s_unlocking)
            {
                // 锁正开着(或正在开)—— 只置个标志,由 lock_tick() 决定在哪一步
                // 收手:OPENING 阶段就在刹车点直接转回锁,保持期就立刻转回锁。
                // 前者避免了"锁舌刚推出去就马上收回来"的机械冲击。
                s_close_req = 1;
                printf("远程关锁请求,已通知状态机\r\n");
            }
            else
            {
                printf("远程关锁请求,但锁本来就是关着的,忽略\r\n");
            }
            break;

        default:
            printf("不认识的锁控命令 %d,忽略\r\n", (int)cmd);
            break;
    }
}

/**
 * @brief  密码锁任务:消费触摸板按键 + 远程锁控命令,并推进锁的状态机
 * @note   为什么要周期性醒来(而不是像以前那样 portMAX_DELAY 死等)?
 *           1) 有两个队列要收(按键、远程命令),不引入队列集就只能轮询;
 *           2) 锁动作中的行程/保持计时,需要有人按节拍推进。
 *         空闲时每 20ms 醒一次,只是看一眼两个空队列,几个微秒的事。
 *         (想彻底不轮询可以开 configUSE_QUEUE_SETS 用队列集,但为这点开销
 *          引一套新机制不划算。)
 * @note   优先级 2,和 cy9cm_scan 同级。远程命令只在队列里等最多一个
 *         LOCK_TICK_MS,不会被 wifi 任务拖慢。
 */
static void lock_task(void *arg)
{
    uint8_t key;
    uint8_t cmd;

    for (;;)
    {
        // ── 远程命令优先 ──
        // 它比按键稀少得多,时效性又强(按了"关锁"就该马上关),
        // 所以每拍先看它。收到就 continue,免得连着两条命令要等两拍。
        if ((lock_cmd_queue != NULL) &&
            (xQueueReceive(lock_cmd_queue, &cmd, 0) == pdPASS))
        {
            lock_handle_remote((lock_cmd_t)cmd);
            continue;
        }

        // ── 再看按键。队列空的话就在这里睡满这一拍 ──
        if (xQueueReceive(cy9cm_key_queue, &key, pdMS_TO_TICKS(LOCK_TICK_MS)) == pdPASS)
        {
            lock_handle_key(key);
            continue;
        }

        // ── 两个队列都空:这一拍用来推进状态机 ──
        // (按键/命令走了 continue 的那几拍不推进也没关系 —— 那些分支都是
        //  几个微秒就返回的,下一拍立刻就到,状态机晚 20ms 推进看不出来)
        lock_tick();
    }
}


uint8_t lock_post_cmd(lock_cmd_t cmd)
{
    uint8_t v = (uint8_t)cmd;

    if (lock_cmd_queue == NULL)
    {
        printf("锁控队列还没建好,命令丢弃\r\n");
        return 0;
    }

    // 超时 0 = 不阻塞。命令排不上队就丢 —— 让 wifi 任务卡在这里等,
    // 只会连累 MQTT 的心跳和接收。
    if (xQueueSend(lock_cmd_queue, &v, 0) != pdPASS)
    {
        printf("锁控队列满,命令丢弃\r\n");
        return 0;
    }

    printf("已投递锁控命令 %d\r\n", (int)cmd);
    return 1;
}


void lock_tasks_init(void)
{
    // 深度 4:远程命令本来就很稀疏,4 条足够吸收"开锁期间连来几条"的突发。
    // 满了 lock_post_cmd() 会丢并打印 —— 锁控讲究时效,排队等不如让云端重发。
    lock_cmd_queue = xQueueCreate(4, sizeof(uint8_t));
    if (lock_cmd_queue == NULL)
    {
        printf("锁控队列创建失败:堆不够了\r\n");
    }

    // 栈 320 字(1280 字节):lock_tick()/lock_verify() 会调到
    // LCD_Font_Dis_Ascii(),它自带 100 字节的 buff,再加上 printf,留足余量。
    xTaskCreate(lock_task, "lock", 320, NULL, 2, NULL);
}


// ══════════════════════════════════════════════════════════════════════════
//  想在这块屏上显示中文怎么办?
//
//  LCD_Font_Dis() 会按字节值分流:字节 < 0xA1 走 ASCII 字库,否则当成 GBK
//  双字节汉字去 W25Q 里查表。而本工程的源码是 UTF-8,一个汉字的 UTF-8 编码
//  是 3 个字节(如 "密" = E5 AF 86),它会被错当成 1.5 个 GBK 汉字去查表,
//  结果是乱码 + 后面的字全部错位。
//
//  正确做法是用 GBK 字节的十六进制转义,这样无论源码是什么编码都不受影响:
//
//      "密码" 的 GBK 是 C3 DC C2 EB        →  "\xC3\xDC\xC2\xEB"
//      "请输入密码" 是 C7 EB CA E4 C8 EB C3 DC C2 EB
//
//  好处:转义序列本身全是 ASCII,不受 armcc 的 GBK 解析和 BOM 影响;
//  坏处:源码里完全看不出写的是什么,必须靠注释对照。
//
//  本项目为了可读性,LCD 上只用 ASCII("PASSWORD" / "UNLOCKED")。
// ══════════════════════════════════════════════════════════════════════════
