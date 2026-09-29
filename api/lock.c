#include "lock.h"
#include "admin.h"      // admin_begin / admin_is_active / admin_handle_key / admin_tick
#include "CY9CM.h"      // cy9cm_key_queue:触摸板按键事件
#include "AT24C04.h"    // 密码存储
#include "L9110S.h"     // 开锁电机
#include "lcd.h"        // 输入界面
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "stdio.h"
#include "string.h"     // memcpy:lock_set_password

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
//
// ⚠ 下面三个 ROW 是**一组对好的数**,要挪位置就整体挪 —— 只改 LOCK_UI_UP
//   这一个值,别去单独改某一个。三条相对关系都是有意的:
//     ① 大提示区从"标题行下沿 + 1"开始(基准 80 + 32 = 112)。一个像素都不碰
//        标题行 —— 于是 lock_tick() 在回锁结束时刷标题、和提示区写字永远
//        不会互盖,不需要引入"屏幕所有权协议"(见 lock.h 里那段说明);
//     ② 密码输入行(基准 160~191)整个落在大提示区的第 2、3 行里 —— 所以进
//        管理员模式时不用整屏清一次就能盖住它,省掉 100ms 以上;
//     ③ 大提示区 6 行 × 24 = 144 行,基准底边 112+144 = 256,离屏底 320 还有 64 行。
//   单独动其中一个,上面至少破一条,现象是"标题被提示区盖掉一半",或者
//   "退出管理员模式后输入行留着半截旧字"。
//
//   基准值(80/160/112)是"界面贴着屏幕中上部"时的数;要整体上下移就改
//   LOCK_UI_UP,往上移写正数、往下移写负数。当前上移 30:
//     标题行 50~81,大提示区 82~225,输入行 130~161。
#define LOCK_UI_UP          30      // 整块界面往上挪多少像素

#define LOCK_LCD_TITLE_ROW  (80  - LOCK_UI_UP)
#define LOCK_LCD_TITLE_COL  72      // 8 个字 × (24/2) = 96 像素,居中
#define LOCK_LCD_INPUT_ROW  (160 - LOCK_UI_UP)
#define LOCK_LCD_INPUT_COL  72      // 6 个字符 × (32/2) = 96 像素,居中
#define LOCK_LCD_TITLE_SIZE 32
#define LOCK_LCD_INPUT_SIZE 32

// ── 大提示区的版面(接口和它为什么存在都写在 lock.h 里)──
//   行高 24、每行 20 个字符:字号 24 时一个 ASCII 字符正好 12 像素宽,
//   20 × 12 = 240 = 屏宽,所以一行刚好铺满,不用算居中。
#define LOCK_BIG_ROW      (112 - LOCK_UI_UP)
#define LOCK_BIG_LINE_H   24
#define LOCK_BIG_LINES    6
#define LOCK_BIG_COLS     20
#define LOCK_BIG_SIZE     24

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
static uint8_t s_lcd_hold;              // 大提示区的占用位(LOCK_SCREEN_xxx 的按位或)

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
 * 🟢 L2 工具代码 —— 契约两条:文字必须是 ASCII(>= 0xA1 的字节会被当 GBK 汉字查字库画成乱码),且靠 LCD_Lock 把整行当一个事务,否则别的任务会把自己的字插进这一行
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
    // 为什么要在这外面再套一层锁(不是多余):
    //   lcd.c 里的 LCD_Set_Point() 已经逐像素自动上锁了,那保证的是
    //   "**每个像素**画在正确的位置上"——不会串位、不会花屏。
    //   但它挡不住另一种乱:本任务刚画完 "PASS",wifi 任务插进来画了
    //   "FINGER  ",回来接着画 "WORD" ——每个字位置都对,可标题行成了
    //   "PASSFINGERWORD"。像素没错,内容错了。
    //   把整串字当一个事务,这一整行才有意义。
    // 互斥量是递归的,内层 LCD_Set_Point 的自动上锁不会死锁。
    LCD_Lock();
    LCD_Font_Dis(row, col, size, fg, bg, text);
    LCD_Unlock();
}

/**
 * 🟢 L2 工具代码 —— 把 s_input_len 翻译成 '*'/'-' 掩码再刷屏;数错一位只是界面难看,校验用的 s_input[] 不受它影响
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
 * 🟢 L2 工具代码 —— 两行复位(长度清零 + 重画),键盘侧和状态机侧都复用它,自己不带任何策略
 * @brief  清空已输入的内容并刷新界面
 */
static void lock_clear_input(void)
{
    s_input_len = 0;
    lock_lcd_input();
}

/**
 * 🟢 L2 工具代码 —— 输出契约固定:标题行红字 ERROR 停 800ms 再变回 PASSWORD;唯一要守的约束是用 vTaskDelay 让出 CPU 而不是 Delay_Ms 忙等
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
 * 🟡 L1 架构代码 —— EEPROM 布局(0xA5 标志 + 长度 + 6 字节密码同页一次写完,不会留半截数据)和"载入 RAM 后不再碰 EEPROM、改密码要重启生效"的生命周期都定在这里
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
 * 🟢 L2 工具代码 —— 契约只有"追加一位、满 6 位就丢";越界写 s_input[] 的可能被开头那行长度判断堵死
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


// 🟢 L2 工具代码 —— 输入输出契约一句话说完:把标题行整行盖成传入文字(少于 8 字符盖不干净,右边会留上一行残影)
void lock_lcd_set_title(uint8_t *title, uint16_t color)
{
    lock_lcd_text(LOCK_LCD_TITLE_ROW, LOCK_LCD_TITLE_COL, LOCK_LCD_TITLE_SIZE,
                  color, WHITE, title);
}

// 🟢 L2 工具代码 —— 无副作用的快照查询,只返回 s_unlocking;它只是给调用方做预检,真正仲裁在 lock_open 的临界区里
uint8_t lock_is_busy(void)
{
    return s_unlocking;
}

/**
 * 🟢 L2 工具代码 —— 只读的状态快照,回答的是"电机这一刻通没通电",和 lock_is_busy() 的"整个开锁流程走没走完"是两回事
 * @brief  电机是不是正通着电(开锁行程 / 回锁行程中)
 * @return 1 = 电机正在转;0 = 电机已刹车(静止,或保持期内已断电)
 * @note   为什么不能复用 lock_is_busy():那个覆盖"开 500ms + 保持 3 秒 + 回
 *         200ms"整整 3.7 秒,而电机真正通电的只有开头 500ms 和结尾 200ms。
 *         保持期内电机早就刹车了,谁在这时候画屏都无所谓。
 * @note   给管理员模式用:它那些整屏重画一次要一秒上下,而 lock_task 同一
 *         时刻只能干一件事 —— 撞上电机行程就会把 L9110S_control(MOTOR_BRAKE)
 *         拖到一秒以后,锁舌顶到头还在转。所以它画之前先问一句,行程中就
 *         欠着,等刹车了再补。
 */
uint8_t lock_motor_running(void)
{
    return ((s_state == LOCK_OPENING) || (s_state == LOCK_CLOSING)) ? 1 : 0;
}

// ── 大提示区(接口说明在 lock.h)──

/**
 * 🟢 L2 工具代码 —— 只画标题行和输入行这两处固定内容,不碰大提示区;幂等,重复调没有副作用
 * @brief  把界面刷回"等着输密码"的静止画面
 * @note   为什么单独抽出来:复位后的初始画面、回锁完成的收尾、管理员模式退出,
 *         三处要画的是同一个东西。原来前两处各写了一遍,现在合并成这一个出口。
 */
void lock_lcd_show_idle(void)
{
    lock_lcd_set_title((uint8_t *)"PASSWORD", BLUE);
    lock_clear_input();
}

/**
 * 🟢 L2 工具代码 —— 契约两条:每行固定 20 个字符(不足自动补空格),这是"这一行能把旧内容整行盖干净"的前提;越界行号直接忽略
 * @brief  在大提示区的第 line 行画一整行文字
 * @param  line 行号 0 ~ LOCK_BIG_LINES-1,越界直接忽略
 * @param  text ASCII 文字,超过 LOCK_BIG_COLS 个字符的部分被截掉
 */
void lock_lcd_big_line(uint8_t line, const char *text)
{
    uint8_t buf[LOCK_BIG_COLS + 1];
    uint8_t i;

    if (line >= LOCK_BIG_LINES)
    {
        return;
    }

    // 先整行铺空格,再把文字盖上去 —— 这一步不只是为了好看:
    // 每个字符格子会被完整写一遍(笔画写前景色、其余写背景色),补齐 20 个字符
    // 才等于把这一行 240 个像素全覆盖,上一屏的字不会在右边留下残影。
    for (i = 0; i < LOCK_BIG_COLS; i++)
    {
        buf[i] = ' ';
    }
    for (i = 0; (i < LOCK_BIG_COLS) && (text[i] != '\0'); i++)
    {
        buf[i] = (uint8_t)text[i];
    }
    buf[LOCK_BIG_COLS] = '\0';

    lock_lcd_text(LOCK_BIG_ROW + (uint16_t)line * LOCK_BIG_LINE_H, 0,
                  LOCK_BIG_SIZE, BLACK, WHITE, buf);
}

/**
 * 🟢 L2 工具代码 —— 一次开窗口批量刷白,比逐行写字快一倍;整块擦干净就完事,不含任何状态
 * @brief  擦掉整个大提示区(刷成白底)
 * @note   左上角是 (LOCK_BIG_ROW, 0),整块 6 行 × 24 像素高 × 240 像素宽。
 */
void lock_lcd_big_clear(void)
{
    LCD_Clear(LOCK_BIG_ROW, LOCK_BIG_ROW + LOCK_BIG_LINES * LOCK_BIG_LINE_H,
              0, 240, WHITE);
}

/**
 * 🟡 L1 架构代码 —— 占用检查与置位必须原子(分开写两个任务会同时以为"没人占"),而"失败就让调用方别画界面、活照干"这条约定决定了 111# 同时派两个活时不会打架
 * @brief  占用大提示区
 * @param  who 占用者标识,取 LOCK_SCREEN_xxx
 * @return 1 = 占到了;0 = 已经有人占着,调用方**活照干但别画界面**
 * @note   检查"有没有人占"和置位必须在同一个临界区里做完。分成两句写的话,
 *         中间被 SysTick 切走一次,两个任务就会双双认为"没人占",然后一起画 ——
 *         111# 初始化同时给指纹和刷卡派活,就是这个场景。
 */
uint8_t lock_screen_take(uint8_t who)
{
    uint8_t got = 0;

    taskENTER_CRITICAL();
    if (s_lcd_hold == 0)
    {
        s_lcd_hold = who;
        got = 1;
    }
    taskEXIT_CRITICAL();

    if (got)
    {
        // 擦一次底:上一家走的时候虽然也擦过,但中间 lock_tick() 有可能
        // 已经往这块里写过密码输入行了(它只认占用位,不认时间先后)
        lock_lcd_big_clear();
    }

    return got;
}

/**
 * 🟡 L1 架构代码 —— 释放时的次序就是设计:先擦屏、再判断要不要刷回静止画面、最后才放占用位,反了会出现"占用位已放、界面还没收尾"的空窗
 * @brief  用屏结束:擦干净大提示区、释放占用位,必要时把界面刷回静止画面
 * @param  who 占用者标识,必须和 lock_screen_take() 传的一致
 */
void lock_screen_give(uint8_t who)
{
    lock_lcd_big_clear();

    // 锁还在动作中的话**不要**去刷标题:那会让 "PASSWORD" 在保持期就显示出来。
    // 这种情况交给 lock_tick() 在回锁完成时收尾 —— 那时候占用位已经放开了,
    // 它那道 if 就能过。
    if (!lock_is_busy() && (s_lcd_hold == who))
    {
        lock_lcd_show_idle();
    }

    taskENTER_CRITICAL();
    s_lcd_hold &= (uint8_t)~who;
    taskEXIT_CRITICAL();
}

/**
 * 🟡 L1 架构代码 —— 改密码的落盘点:EEPROM 和 RAM 必须一起更新,只写一边就会出现"这次能用、重启后变回去"
 * @brief  改密码:写进 EEPROM,并立刻更新 RAM 里的缓存
 * @param  pwd 新密码(ASCII)
 * @param  len 长度,必须等于 LOCK_PWD_LEN
 * @return 1 = 成功;0 = 长度不对 或 EEPROM 写失败
 */
uint8_t lock_set_password(const uint8_t *pwd, uint8_t len)
{
    uint8_t buf[LOCK_EEPROM_BYTES];
    uint8_t i;

    if (len != LOCK_PWD_LEN)
    {
        printf("改密码:长度必须是 %d 位\r\n", LOCK_PWD_LEN);
        return 0;
    }

    buf[0] = LOCK_FLAG_VALUE;
    buf[1] = LOCK_PWD_LEN;
    for (i = 0; i < LOCK_PWD_LEN; i++)
    {
        buf[2 + i] = pwd[i];
    }

    // ⚠ 这一句里有约 20ms 的 NOP 忙等(见 lock.h 的说明)。写失败就**不动**
    //   RAM 里的 s_pwd[]:免得这次能用、掉电重启之后又变回旧密码,让人以为
    //   "明明改过了却打不开"。
    if (AT24C04_page_write(LOCK_EEPROM_ADDR, buf, LOCK_EEPROM_BYTES) != 0)
    {
        printf("改密码:写 EEPROM 失败,新密码不生效\r\n");
        return 0;
    }

    for (i = 0; i < LOCK_PWD_LEN; i++)
    {
        s_pwd[i] = pwd[i];
    }

    printf("密码已更新,立即生效\r\n");
    return 1;
}

/**
 * 🟡 L1 架构代码 —— 状态机入口的次序就是设计:先写 s_state 再刷 LCD/推电机,避免出现 s_unlocking=1 而 s_state=IDLE 的自相矛盾窗口,并清掉上一轮的 s_close_req
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

    // 开锁行程:正转推出去,转多久由 lock_tick() 按 LOCK_MOTOR_OPEN_MS 决定 ——
    // 万一行程中途收到关锁命令,那边刹车后会直接转 CLOSING。
    // (上面刷 LCD 花掉的几毫秒会算进行程里,几百 ms 的尺度上无所谓)
    L9110S_control(MOTOR_FORWARD);

    printf("开锁中,保持 %d 秒后自动回锁(期间收到关锁命令会立刻回锁)\r\n",
           LOCK_OPEN_MS / 1000);
}

/**
 * 🟡 L1 架构代码 —— 自动回锁和远程关锁共用的唯一出口:先置 CLOSING 再反转电机,维持"s_state 与实际动作一致"这条不变量
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
 * 🟡 L1 架构代码 —— 四个状态的转移和刹车时机全在这一个 switch 里;关锁请求在 OPENING(等到刹车点)和 OPEN(立即)的不同落点,就是"保持期可被打断"的实现
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
            // 推锁舌 —— 用"开锁"的行程时间
            if ((now - s_phase_start) >= LOCK_MOTOR_OPEN_MS)
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
            // 收锁舌 —— 用"回锁"的行程时间(和开锁是两个独立可调的值)
            if ((now - s_phase_start) >= LOCK_MOTOR_CLOSE_MS)
            {
                L9110S_control(MOTOR_BRAKE);
                s_state = LOCK_IDLE;

                // 有大提示区被占着(管理员模式/指纹录入/刷卡录入/蓝牙配网)就别去
                // 刷界面:那几个流程是在锁还没动作完时就开始往屏上写字的,这一刷
                // 会把它们的提示盖掉 —— 而且刷的是密码输入行(row 160~191),正好
                // 落在大提示区的第二、三行里,盖出来的是一屏花字。
                // 等它们放手(lock_screen_give)时自己会补上这一下。
                if (s_lcd_hold == 0)
                {
                    lock_lcd_show_idle();
                }

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
 * 🟡 L1 架构代码 —— 临界区里做 check-and-set 抢 s_unlocking 开锁标志,分开写会让两个来源(指纹/云端)同时推电机;非阻塞返回是远程关锁能及时被响应的前提
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
 * 🟡 L1 架构代码 —— 业务裁决点:位数不足也算错,先清输入行再闪红字(反了会看着像"密码还在,但已经错了");通过后走 lock_open 公共出口而不自己碰电机
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

    // 开锁受理了才进管理员模式 —— 被拒(已经有别的开锁流程在跑)说明这次不是
    // "我刚开的锁",不该把管理员权限交出去。
    if (lock_open((uint8_t *)"UNLOCKED", GREEN))
    {
        admin_begin();
    }
}


/**
 * 🟡 L1 架构代码 —— 交互策略集中点:'0'~'9'/'*'/'#' 各自去哪、开锁期间一律吞掉,行为规则整套由这一个 if-else 链定义
 * @brief  处理一个触摸板按键
 * @param  key '0'~'9' / '*' / '#'
 * @note   开锁/回锁过程中一律忽略 —— 那时 LCD 上显示的是 "UNLOCKED",
 *         再让按键去改输入行只会把界面搞乱。
 */
static void lock_handle_key(uint8_t key)
{
    // ⚠ 管理员模式必须排在下面 s_unlocking 那行**之前**。
    //   它的窗口是"开锁的同时"打开的(见 admin.h),此刻 s_unlocking 正好是 1;
    //   排在后面的话,这 4 秒里所有的键都会被下面这行原样吞掉,
    //   111#/222#/333#/444# 一个也按不进去。
    if (admin_is_active())
    {
        admin_handle_key(key);
        return;
    }

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
 * 🟡 L1 架构代码 —— 云端命令的分流策略:开锁借 lock_open 公共出口(忙则拒并打印原因),关锁只置 s_close_req 让 lock_tick 选刹车时机,绝不直接动电机
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
 * 🟡 L1 架构代码 —— 任务与队列结构:远程命令优先→按键次之→两队列都空才推进状态机,20ms 一拍是"命令及时"与"不空转"的折中
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
        // 它比按键稀少得多,时效性又强(按了"关锁"就该马上关),所以每拍先看它。
        if ((lock_cmd_queue != NULL) &&
            (xQueueReceive(lock_cmd_queue, &cmd, 0) == pdPASS))
        {
            lock_handle_remote((lock_cmd_t)cmd);
        }
        // ── 再看按键。两个队列都空的话,就在这里睡满这一拍 ──
        else if (xQueueReceive(cy9cm_key_queue, &key, pdMS_TO_TICKS(LOCK_TICK_MS)) == pdPASS)
        {
            lock_handle_key(key);
        }

        // ── 不管这一拍是收了命令、收了按键、还是睡过去的,都要推这两件事 ──
        //
        // ⚠ lock_tick() 必须排在 admin_tick() **前面**。它管的是电机的刹车,
        //   是全系统最不能等的一件事;而 admin_tick() 一旦要画屏,一次就是
        //   一两百毫秒(大提示区一行 = 20 个字符 × 24×12 像素,而底层走的是
        //   LCD_Set_Point —— 一个像素开一次 ST7789 地址窗口,慢得离谱)。
        //   让画屏排在刹车前面,锁舌就会顶到头还继续转,堵转发热。
        //
        //   原先这里只有"两个队列都空"那一拍才推,而且 admin_tick() 在前、
        //   lock_tick() 在后,理由是"按键分支几个微秒就返回,晚一拍看不出
        //   来"。管理员模式把那个前提推翻了:密码开锁成功那一刻,
        //   admin_begin() 就在同一条调用链上
        //   (lock_handle_key → lock_verify → lock_open → admin_begin),
        //   当时它是同步画整屏菜单的,于是 lock_tick() 整整一秒回不来 ——
        //   上板现象就是"密码开锁后电机一直转,时间远远超过
        //   LOCK_MOTOR_OPEN_MS"。现在两头一起堵:这里每拍都推、刹车排在前面,
        //   admin.c 那边则把所有画屏欠到 lock_motor_running() == 0 之后才补。
        lock_tick();
        admin_tick();
    }
}


// 🟡 L1 架构代码 —— 投递策略是"非阻塞、满就丢":xQueueSend 超时填 0,宁丢命令也不让 wifi 任务卡住拖垮 MQTT 心跳
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


// 🟡 L1 架构代码 —— 资源额度在这里定死:队列深 4 吸收突发、任务栈 320 字给 LCD 字库查找的 100 字节 buff 留余量,分小了就是丢命令或栈溢出
void lock_tasks_init(void)
{
    // 深度 4:远程命令本来就很稀疏,4 条足够吸收"开锁期间连来几条"的突发。
    // 满了 lock_post_cmd() 会丢并打印 —— 锁控讲究时效,排队等不如让云端重发。
    lock_cmd_queue = xQueueCreate(4, sizeof(uint8_t));
    if (lock_cmd_queue == NULL)
    {
        printf("锁控队列创建失败:堆不够了\r\n");
    }

    // 栈 384 字(1536 字节)。
    //   原来是 320:lock_tick()/lock_verify() 会调到 LCD_Font_Dis_Ascii(),
    //   它自带 100 字节的 buff,再加上 printf,320 就够。
    //   加管理员模式之后调用链深了一层 ——
    //     lock_task → lock_handle_key → admin_handle_key → admin_submit
    //               → admin_show_msg → lock_lcd_big_line(自带 21 字节 buf)
    //               → lock_lcd_text → LCD_Font_Dis → LCD_Font_Dis_Ascii
    //   比原来多了两三层栈帧。栈溢出会被 vApplicationStackOverflowHook 抓住,
    //   而那个钩子是"删掉任务再死循环",上板表现是"按了没反应、串口也不打了",
    //   极难往栈上想。多切 256 字节(堆还剩 1.6KB 以上)换掉这个隐患是划算的。
    xTaskCreate(lock_task, "lock", 384, NULL, 2, NULL);
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
