#include "admin.h"
#include "lock.h"       // LOCK_PWD_LEN / lock_screen_* / lock_lcd_big_* / lock_set_password
#include "finger.h"     // finger_request_enroll / finger_request_erase
#include "rc522_task.h" // rc522_request_learn / rc522_request_erase
#include "wifi.h"       // wifi_request_provision
#include "app_config.h" // APP_USE_FINGER / APP_USE_RFID / APP_USE_WIFI
#include "FreeRTOS.h"
#include "task.h"
#include "stdio.h"
#include "string.h"

// 本模块的完整设计说明(为什么入口是密码开锁、为什么必须不阻塞、长活派给谁)
// 都在 admin.h 的文件头,改之前先读那一大段。

// ── 版面 ──
//   大提示区一共 6 行(行 0~5),这里用行 0 当标题、行 1~4 放五条子模式说明
//   (333# 和 444# 挤一行)、行 5 专门用来回显"已经按了几个键"。
//   行号越界 lock_lcd_big_line() 会自己忽略。
#define ADMIN_LINE_TITLE   0
#define ADMIN_LINE_1       1
#define ADMIN_LINE_2       2
#define ADMIN_LINE_3       3
#define ADMIN_LINE_4       4
#define ADMIN_LINE_ECHO    5

// 大提示区一共几行 —— 整屏重画按行推进,画满这么多行算画完
#define ADMIN_LINE_COUNT   6

// 回显行 "> 123" 最长 = 2 个前缀 + LOCK_PWD_LEN 个字符
#define ADMIN_ECHO_MAX     (2 + LOCK_PWD_LEN)

// ADMIN_MSG 要显示的两行文字暂存在这里(大提示区一行 20 个字符,留一个 '\0')
#define ADMIN_MSG_MAX      (20 + 1)

// ── 子模式代码 ──
//   写成宏而不是散在 if 里的字面量:这几个串同时出现在"比较"和"菜单显示"
//   两处,分开写迟早会出现"菜单上写着 333#、代码里比的是 334#"。
#define ADMIN_CODE_CLEAR   "111"
#define ADMIN_CODE_NEWPWD  "222"
#define ADMIN_CODE_CARD    "333"
#define ADMIN_CODE_FINGER  "444"
#define ADMIN_CODE_WIFI    "555"
#define ADMIN_CODE_LEN     3

// 子模式代码用 '#' 结束。< 0xA1 的字符都走 ASCII 字库,所以这几个符号在屏上
// 显示不会有问题(中文才需要 GBK 转义,见 lock.c 末尾)。

/**
 * ── 状态 ──
 *   注意这些变量**只在 lock_task 里读写**(admin_handle_key / admin_tick /
 *   admin_begin 都是从 lock_task 的调用链上进来的),所以不需要临界区。
 *   唯一跨任务的是派给 finger/rc522 的那两个请求标志,它们在自己的模块里保护。
 */
typedef enum
{
    ADMIN_OFF = 0,      // 没开
    ADMIN_MENU,         // 菜单窗口:等 111#/222#/333#/444#
    ADMIN_NEWPWD_1,     // 222# 第一步:输第一遍新密码
    ADMIN_NEWPWD_2,     // 222# 第二步:再输一遍确认
    ADMIN_MSG           // 结果提示停留中,到点自动退出
} admin_state_t;

static admin_state_t s_state = ADMIN_OFF;
static uint32_t      s_last_key;                    // 最后一次按键的时刻
static uint8_t       s_buf[LOCK_PWD_LEN];           // 已经按下的键
static uint8_t       s_len;                         // 已经按了几位
static uint8_t       s_new1[LOCK_PWD_LEN];          // 222# 的第一遍新密码

// ── 欠着的画面 ──
//   为什么画面要"欠着"而不是就地画,见 admin.h 文件头 —— 画屏一次就是
//   一两百毫秒,按键路径上画屏会把 lock_tick() 的电机刹车一起拖住。
//   所以按键处理只登记(admin_set_redraw),真正落笔全在 admin_flush_redraw()。
#define ADMIN_REDRAW_NONE  0        // 没有欠账
#define ADMIN_REDRAW_ECHO  1        // 只欠回显行
#define ADMIN_REDRAW_FULL  2        // 整屏都欠着,按行补

static uint8_t       s_redraw;                      // ADMIN_REDRAW_xxx
static uint8_t       s_redraw_line;                 // 整屏重画补到第几行了
static uint8_t       s_has_screen;                  // 大提示区是不是我们占着
static const char   *s_echo_msg;                    // 非 NULL 时回显行显示这句提示
static char          s_msg2[ADMIN_MSG_MAX];         // ADMIN_MSG 的第三行文字
static char          s_msg3[ADMIN_MSG_MAX];         // ADMIN_MSG 的第四行文字

/**
 * 🟢 L2 工具代码 —— 纯画屏,没有状态;唯一的约定是"不足的位数补空格,由 lock_lcd_big_line 负责",这里只管拼前缀和掩码
 * @brief  在回显行显示已经输入的按键
 * @param  masked 1 = 显示成 '*' (输密码用),0 = 原样显示 (输子模式代码用)
 * @note   子模式代码不遮:111#/222# 这些不是秘密,遮起来反而让人不知道自己
 *         按到第几位了。密码要遮,原因和登录界面上一屏幕 '*' 一样。
 */
static void admin_lcd_echo(uint8_t masked)
{
    char line[ADMIN_ECHO_MAX + 1];
    uint8_t i;

    line[0] = '>';
    line[1] = ' ';

    for (i = 0; i < s_len; i++)
    {
        line[2 + i] = masked ? '*' : (char)s_buf[i];
    }
    line[2 + s_len] = '\0';

    lock_lcd_big_line(ADMIN_LINE_ECHO, line);
}

/**
 * 🟢 L2 工具代码 —— 纯画屏,只画回显行这一行;s_echo_msg 有货时它压过 "> 111"
 * @brief  画回显行(菜单最下面那一行)
 * @note   "NEED 3 DIGITS" / "BAD CODE" 这类一次性提示借的就是回显行的位置。
 *         让它压过 "> 111" 是有意的:用户此刻需要知道的是"刚才那次为什么
 *         不算数",而不是自己按到第几位了。下一次按键会把 s_echo_msg 清掉。
 */
static void admin_lcd_echo_line(void)
{
    if (s_echo_msg != NULL)
    {
        lock_lcd_big_line(ADMIN_LINE_ECHO, s_echo_msg);
        return;
    }

    admin_lcd_echo((s_state == ADMIN_MENU) ? 0 : 1);
}

/**
 * 🟢 L2 工具代码 —— "当前状态该显示什么"的单行版:只画第 line 行,行号越界直接忽略
 * @brief  按当前状态画大提示区的第 line 行
 * @param  line 0 ~ ADMIN_LINE_COUNT-1(最后一行留给回显行)
 * @note   为什么拆成"一次一行"而不是六行一次画完:一行是 240×24 = 5760 个像素,
 *         而底层 LCD_Set_Point() 是**一个像素开一次 ST7789 地址窗口**的写法,
 *         一行要一两百毫秒,六行就是一秒多。这一秒里 lock_task 收不了按键、
 *         也收不了远程命令(远程"关锁"要等它一秒)。拆开之后每两行之间都能
 *         回去收一次,响应快一个数量级。
 * @note   画哪张脸由 s_state 决定,**所有** lock_lcd_big_line() 调用都收在这一个
 *         函数里 —— 别处再直接调就绕过了"按键路径不许碰 LCD"那条规矩。
 */
static void admin_redraw_line(uint8_t line)
{
    switch (s_state)
    {
        // ── 主菜单:5 个子模式的进入方法 ──
        //   5 条说明要塞进 4 行,所以 333# 和 444# 挤一行 —— 它们本来就是
        //   一类的(都是"加一个开锁凭据"),并排反而好记。
        //   ⚠ 大提示区每行只有 20 个字符(见 lock.h),改文案要数一下,
        //     超出的部分会被 lock_lcd_big_line() 直接截掉。
        case ADMIN_MENU:
            switch (line)
            {
                case ADMIN_LINE_TITLE: lock_lcd_big_line(line, "ADMIN MODE");    break;
                case ADMIN_LINE_1:     lock_lcd_big_line(line, ADMIN_CODE_CLEAR  "# CLEAR ALL");       break;
                case ADMIN_LINE_2:     lock_lcd_big_line(line, ADMIN_CODE_NEWPWD "# SET PASSWORD");    break;
                case ADMIN_LINE_3:     lock_lcd_big_line(line, ADMIN_CODE_CARD   "#CARD " ADMIN_CODE_FINGER "#FINGER"); break;
                case ADMIN_LINE_4:     lock_lcd_big_line(line, ADMIN_CODE_WIFI   "# BLUETOOTH PROV");  break;
                default:               admin_lcd_echo_line();                                          break;
            }
            break;

        // ── 222# 的两步:界面一样,只有第二行那句提示不同 ──
        case ADMIN_NEWPWD_1:
        case ADMIN_NEWPWD_2:
            switch (line)
            {
                case ADMIN_LINE_TITLE: lock_lcd_big_line(line, "SET PASSWORD"); break;
                case ADMIN_LINE_1:     lock_lcd_big_line(line, "");             break;
                case ADMIN_LINE_2:     lock_lcd_big_line(line, (s_state == ADMIN_NEWPWD_1) ?
                                                             "TYPE 6 DIGITS" : "TYPE IT AGAIN");
                                       break;
                case ADMIN_LINE_3:     lock_lcd_big_line(line, "THEN PRESS #"); break;
                case ADMIN_LINE_4:     lock_lcd_big_line(line, "");             break;
                default:               admin_lcd_echo_line();                   break;
            }
            break;

        // ── 结果提示:只占第三、四行,其余刷空 ──
        case ADMIN_MSG:
            if (line == ADMIN_LINE_2)
            {
                lock_lcd_big_line(line, s_msg2);
            }
            else if (line == ADMIN_LINE_3)
            {
                lock_lcd_big_line(line, s_msg3);
            }
            else
            {
                lock_lcd_big_line(line, "");
            }
            break;

        default:
            break;
    }
}

/**
 * 🟢 L2 工具代码 —— 欠画面的唯一登记口,把"整屏重画优先于只补回显行"这条合并规则收在一处
 * @brief  登记"欠了一屏画面",等 admin_tick() 去补
 * @param  level ADMIN_REDRAW_ECHO(只欠回显行)或 ADMIN_REDRAW_FULL(整屏)
 */
static void admin_set_redraw(uint8_t level)
{
    if (level == ADMIN_REDRAW_FULL)
    {
        s_redraw      = ADMIN_REDRAW_FULL;
        s_redraw_line = 0;      // 从头一行一行画
        return;
    }

    // 整屏重画已经欠着的话,不用再单独登记回显行 —— 整屏的最后一行就是回显行,
    // 而且它取的是**补画那一刻**的 s_buf,比现在登记的还新。
    if (s_redraw != ADMIN_REDRAW_FULL)
    {
        s_redraw = ADMIN_REDRAW_ECHO;
    }
}

/**
 * 🟡 L1 架构代码 —— 状态与画面必须一起改:先切到 ADMIN_MSG 再画字,否则这中间来的按键会被当成菜单输入;到点由 admin_tick() 收工
 * @brief  只显示一条结果,停留 ADMIN_MSG_MS 之后自动退出管理员模式
 * @param  l2 第三行(结果的正文)
 * @param  l3 第四行(补充说明,不需要就传空串)
 * @note   为什么要"停留一会"而不是直接退出:admin_exit() 第一件事就是把大提示区
 *         擦干净,直接退的话用户什么都看不到,只看见屏幕闪一下。
 * @note   文字先抄进 s_msg2/s_msg3 而不是直接画 —— 这个函数是从按键路径
 *         (admin_submit)进来的,那时候电机可能还在转,画屏得欠着。
 * @note   s_msg2/s_msg3 是各 21 字节的定长缓冲,所以传进来的串超长会被截到
 *         20 个字符。这里**故意**不打印截断警告:大提示区一行本来就只有 20 个
 *         字符的宽度,超出的部分画上去也会被 lock_lcd_big_line() 截掉,两处
 *         截法一致,截断本身没有副作用。
 */
static void admin_show_msg(const char *l2, const char *l3)
{
    strncpy(s_msg2, l2, ADMIN_MSG_MAX - 1);
    s_msg2[ADMIN_MSG_MAX - 1] = '\0';

    strncpy(s_msg3, l3, ADMIN_MSG_MAX - 1);
    s_msg3[ADMIN_MSG_MAX - 1] = '\0';

    s_state    = ADMIN_MSG;
    s_len      = 0;
    s_last_key = xTaskGetTickCount();

    admin_set_redraw(ADMIN_REDRAW_FULL);
}

/**
 * 🟡 L1 架构代码 —— 退出的次序就是设计:先改状态再收尾,收尾那一步把状态机"锁还在动作中就别刷标题"的判断交给 lock_screen_give 统一处理
 * @brief  退出管理员模式:擦掉菜单、放开屏幕、必要时把界面刷回静止画面
 * @note   挂在这里的还有一件事:如果锁的动作在做管理员模式的这段时间里已经
 *         结束了,lock_tick() 那一趟是被占用位挡住的(它不会刷界面),所以
 *         **必须由 lock_screen_give() 补上那一下**,否则标题会一直停在 "UNLOCKED"。
 */
/**
 * 🟢 L2 工具代码 —— 把状态机清回"没开"的样子,不碰屏幕也不打印;屏幕那边的收尾是别人的事
 * @brief  清空管理员模式的所有状态
 * @note   和 admin_exit() 的分工:这个不动屏幕占用位。给"根本没抢到屏"的那条
 *         路径用 —— 那时候 lock_screen_give() 是多余的,它内部那次约 50ms 的
 *         擦屏纯属白花;而且它还会顺带判断要不要刷回静止画面,更容易把屏幕
 *         搞乱。一条铁律:**没 take 过就不许 give**。
 */
static void admin_reset(void)
{
    s_state    = ADMIN_OFF;
    s_len      = 0;
    s_redraw   = ADMIN_REDRAW_NONE;
    s_echo_msg = NULL;
}

static void admin_exit(void)
{
    if (s_state == ADMIN_OFF)
    {
        return;
    }

    admin_reset();

    if (s_has_screen)
    {
        s_has_screen = 0;
        lock_screen_give(LOCK_SCREEN_ADMIN);
    }

    printf("已退出管理员模式\r\n");
}

/**
 * 🟡 L1 架构代码 —— 不阻塞画屏的落地点:电机通着电就继续欠着,欠账期间按键照收不误;整屏重画一次只画一行,让 lock_task 每两行之间都能回去收按键和远程命令
 * @brief  把欠着的画面补上一点点 —— 由 admin_tick() 每拍调一次
 * @note   两条前提缺一不可(见 admin.h 文件头):
 *           ① lock_motor_running() == 0 —— 电机在转就先别画,刹车最要紧;
 *           ② 一次最多画一行            —— 一行也要一两百毫秒。
 *         少哪一条都会退回原来那个"开锁时间远远超过 LOCK_MOTOR_OPEN_MS"的毛病。
 * @note   抢屏也挪到了这里:第一次真要落笔的时候才 lock_screen_take()。
 *         抢不到就整个放弃 —— 屏幕正被别的录入流程占着,硬画只会两边互相盖。
 *         注意这时**没有**占过屏,所以走 admin_reset() 而不是 admin_exit()。
 * @note   拿到屏之后强制改成整屏重画:lock_screen_take() 会把整块擦成白底,
 *         只补一行的话剩下五行是白的。反正第一次落笔必然是整屏都要。
 */
static void admin_flush_redraw(void)
{
    if (s_redraw == ADMIN_REDRAW_NONE)
    {
        return;
    }

    // 电机正通着电:继续欠着。用户这几百毫秒里敲的键照样被收下,等刹车了
    // 一次性画出来 —— 屏幕晚几百毫秒,总好过锁舌堵转。
    if (lock_motor_running())
    {
        return;
    }

    if (!s_has_screen)
    {
        if (!lock_screen_take(LOCK_SCREEN_ADMIN))
        {
            printf("管理员:屏幕正被录入流程占着,本次不开管理员模式\r\n");
            admin_reset();
            return;
        }

        s_has_screen = 1;

        // 刚把整块擦成白底,必须从头整屏重画
        s_redraw      = ADMIN_REDRAW_FULL;
        s_redraw_line = 0;
    }

    if (s_redraw == ADMIN_REDRAW_ECHO)
    {
        s_redraw = ADMIN_REDRAW_NONE;
        admin_lcd_echo_line();
        return;
    }

    // ── 整屏重画:这一拍只画一行 ──
    if (s_redraw_line >= ADMIN_LINE_COUNT)
    {
        s_redraw = ADMIN_REDRAW_NONE;
        return;
    }

    admin_redraw_line(s_redraw_line);
    s_redraw_line++;

    if (s_redraw_line >= ADMIN_LINE_COUNT)
    {
        s_redraw = ADMIN_REDRAW_NONE;

        // ADMIN_MSG 的"停留 ADMIN_MSG_MS"要从**画完那一刻**起算,不是从按下
        // '#' 那一刻起算。整屏 6 行画完本身就要一秒上下,按按下时刻算的话提示
        // 刚露脸就到点了(1.5 秒的额度里 1.2 秒花在画它自己身上)。
        // 这样 222# 改密码的结果("PASSWORD CHANGED"/"MISMATCH")才真的看得清。
        if (s_state == ADMIN_MSG)
        {
            s_last_key = xTaskGetTickCount();
        }
    }
}

/**
 * 🟡 L1 架构代码 —— 按键语义的裁决点:哪几个键算"输满"、哪些代码算有效子模式、每个子模式是"自己做"还是"派活走人",整套规则都在这一个函数里
 * @brief  按下 '#' :解释已经输入的那一串按键
 */
static void admin_submit(void)
{
    /* ── 主菜单:期望 3 位子模式代码 ── */
    if (s_state == ADMIN_MENU)
    {
        if (s_len != ADMIN_CODE_LEN)
        {
            s_len      = 0;
            s_echo_msg = "NEED 3 DIGITS";
            admin_set_redraw(ADMIN_REDRAW_ECHO);
            return;
        }

        /* ---- 111# 初始化:清空指纹库 + 删除授权卡号 ---- */
        if (memcmp(s_buf, ADMIN_CODE_CLEAR, ADMIN_CODE_LEN) == 0)
        {
            printf("管理员:%s# 初始化 —— 清空指纹库 + 删除授权卡号\r\n", ADMIN_CODE_CLEAR);

            // 退出的次序见 admin.h 文件头那条 ⚠:先把屏还回去,再派活。
            admin_exit();

            // 两件活分给两个任务:它们各自拥有那个外设(MG200 归 finger_task,
            // RC522 归 rc522_task),这里只投递意图。
            // 屏幕上谁抢到谁显示,没抢到的默默干活 —— 否则两个任务会往同一块
            // 屏上互相盖(lock_screen_take 保证只有一个能抢到)。
#if APP_USE_FINGER
            finger_request_erase();
#else
            printf("管理员:APP_USE_FINGER = 0,指纹没编进来,跳过清空指纹\r\n");
#endif

#if APP_USE_RFID
            rc522_request_erase();
#else
            printf("管理员:APP_USE_RFID = 0,刷卡没编进来,跳过删卡\r\n");
#endif

            return;
        }

        /* ---- 222# 改密码:这个全程是键盘输入,admin 自己做 ---- */
        if (memcmp(s_buf, ADMIN_CODE_NEWPWD, ADMIN_CODE_LEN) == 0)
        {
            printf("管理员:%s# 修改密码 —— 请输入新密码\r\n", ADMIN_CODE_NEWPWD);

            s_state    = ADMIN_NEWPWD_1;
            s_len      = 0;
            s_last_key = xTaskGetTickCount(); // 从这里起改按"子模式超时"计时
            s_echo_msg = NULL;
            admin_set_redraw(ADMIN_REDRAW_FULL);
            return;
        }

        /* ---- 333# 加卡:派给 rc522_task,它才持有 RC522 ---- */
        if (memcmp(s_buf, ADMIN_CODE_CARD, ADMIN_CODE_LEN) == 0)
        {
            printf("管理员:%s# 增加卡 —— 请把卡放到感应区\r\n", ADMIN_CODE_CARD);
            admin_exit();
#if APP_USE_RFID
            rc522_request_learn();
#else
            printf("管理员:APP_USE_RFID = 0,刷卡没编进来,无法加卡\r\n");
#endif
            return;
        }

        /* ---- 444# 加指纹:派给 finger_task,它才持有 MG200 ---- */
        if (memcmp(s_buf, ADMIN_CODE_FINGER, ADMIN_CODE_LEN) == 0)
        {
            printf("管理员:%s# 添加指纹 —— 请把手指放上去\r\n", ADMIN_CODE_FINGER);
            admin_exit();
#if APP_USE_FINGER
            finger_request_enroll();
#else
            printf("管理员:APP_USE_FINGER = 0,指纹没编进来,无法加指纹\r\n");
#endif
            return;
        }

        /* ---- 555# 蓝牙配网:派给 wifi_task,它才持有 USART2 和那套 AT 状态机 ---- */
        if (memcmp(s_buf, ADMIN_CODE_WIFI, ADMIN_CODE_LEN) == 0)
        {
            printf("管理员:%s# 蓝牙配网 —— 手机装 ESP BluFi App 去改 WiFi\r\n", ADMIN_CODE_WIFI);
            admin_exit();
#if APP_USE_WIFI
            wifi_request_provision();
#else
            printf("管理员:APP_USE_WIFI = 0,联网没编进来,无法配网\r\n");
#endif
            return;
        }

        printf("管理员:无效代码 %c%c%c\r\n", s_buf[0], s_buf[1], s_buf[2]);
        s_len      = 0;
        s_echo_msg = "BAD CODE";
        admin_set_redraw(ADMIN_REDRAW_ECHO);
        return;
    }

    /* ── 222# 第一步:收下第一遍新密码 ── */
    if (s_state == ADMIN_NEWPWD_1)
    {
        if (s_len != LOCK_PWD_LEN)
        {
            s_len      = 0;
            s_echo_msg = "NEED 6 DIGITS";
            admin_set_redraw(ADMIN_REDRAW_ECHO);
            return;
        }

        memcpy(s_new1, s_buf, LOCK_PWD_LEN);
        s_state    = ADMIN_NEWPWD_2;
        s_len      = 0;
        s_last_key = xTaskGetTickCount();

        printf("管理员:请再输一遍确认\r\n");
        s_echo_msg = NULL;
        admin_set_redraw(ADMIN_REDRAW_FULL);
        return;
    }

    /* ── 222# 第二步:两遍一致才写 EEPROM ── */
    if (s_state == ADMIN_NEWPWD_2)
    {
        if (s_len != LOCK_PWD_LEN)
        {
            s_len      = 0;
            s_echo_msg = "NEED 6 DIGITS";
            admin_set_redraw(ADMIN_REDRAW_ECHO);
            return;
        }

        if (memcmp(s_buf, s_new1, LOCK_PWD_LEN) != 0)
        {
            // 两遍不一致就整个作废,不保留第一遍 —— 保留的话用户得猜
            // "刚才那次算不算数",不如退出来重来一次干净。
            printf("管理员:两次输入不一致,密码未改\r\n");
            admin_show_msg("MISMATCH", "PASSWORD UNCHANGED");
            return;
        }

        // lock_set_password() 会同时更新 EEPROM 和 RAM,写完立刻生效。
        // ⚠ 这里面有约 20ms 的 EEPROM 忙等,对本任务(以及和它同优先级的
        //   指纹/刷卡任务)来说就是晚 20ms 响应一次按键,感觉不出来。
        if (lock_set_password(s_new1, LOCK_PWD_LEN))
        {
            admin_show_msg("PASSWORD CHANGED", "");
        }
        else
        {
            // 写失败时 lock_set_password() 不会动 RAM 里的旧密码,
            // 也就是说旧密码仍然有效 —— 这一点要在屏上说清楚,不然用户会
            // 以为"新密码没生效 = 锁坏了"。
            admin_show_msg("SAVE FAILED", "OLD PWD STILL OK");
        }
        return;
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  对外的四个入口
// ══════════════════════════════════════════════════════════════════════════

/**
 * 🟡 L1 架构代码 —— 它只被 lock_verify() 在开锁成功之后调一次,这个时机决定了"谁能进管理员模式";窗口没能独占屏幕时会主动放弃,不会和正在进行的录入流程抢屏
 * @brief  进入管理员模式
 */
void admin_begin(void)
{
    if (s_state != ADMIN_OFF)
    {
        return; // 已经开着了
    }

    s_state    = ADMIN_MENU;
    s_len      = 0;
    s_last_key = xTaskGetTickCount();
    s_echo_msg = NULL;

    // ⚠ 菜单**欠着**,这里一个像素都不画 —— 调用它的那一刻电机刚正转起来,
    //   lock_tick() 要到 LOCK_MOTOR_OPEN_MS 才刹车,同步画整屏会把刹车拖到
    //   一秒以后(这就是"密码开锁后电机一直转"的根因)。抢屏和落笔都在
    //   admin_flush_redraw() 里,等 lock_motor_running() == 0 再补。
    admin_set_redraw(ADMIN_REDRAW_FULL);

    printf("进入管理员模式:%d 秒内输入 %s# / %s# / %s# / %s# / %s#\r\n",
           ADMIN_WINDOW_MS / 1000,
           ADMIN_CODE_CLEAR, ADMIN_CODE_NEWPWD, ADMIN_CODE_CARD,
           ADMIN_CODE_FINGER, ADMIN_CODE_WIFI);
}

// 🟢 L2 工具代码 —— 无副作用的快照查询,唯一作用是让 lock.c 决定"这个键该给谁"
uint8_t admin_is_active(void)
{
    return (s_state != ADMIN_OFF) ? 1 : 0;
}

/**
 * 🟡 L1 架构代码 —— 按键分流的接管点:它一开着,lock.c 的密码输入逻辑就整个让位;必须能在 s_unlocking=1 时工作,否则这 4 秒形同虚设
 * @brief  把一个触摸板按键交给管理员模式
 * @param  key '0'~'9' / '*' / '#'
 * @note   ⚠ 这个函数(以及它调到的 admin_submit)**一个像素都不许画**,
 *         只改状态 + 登记 s_redraw。原因见 admin.h 文件头:它跑在 lock_task
 *         的调用链上,画屏就会把 lock_tick() 的电机刹车一起拖住。
 */
void admin_handle_key(uint8_t key)
{
    uint8_t need;

    if (s_state == ADMIN_OFF)
    {
        return; // 没开着就不该被调到这里;真被调到了也不该吞掉这个键
    }

    // 结果提示停留期间不再收键 —— 它自己到点就会退出,这时候再按键只会让
    // 用户以为"还能接着操作"。
    if (s_state == ADMIN_MSG)
    {
        return;
    }

    // 每次按键都把倒计时往后推,见 admin.h 里 ADMIN_WINDOW_MS 的说明
    s_last_key = xTaskGetTickCount();

    // 菜单里只收 3 位(子模式代码),输密码时要收 6 位
    need = (s_state == ADMIN_MENU) ? ADMIN_CODE_LEN : LOCK_PWD_LEN;

    if ((key >= '0') && (key <= '9'))
    {
        if (s_len >= need)
        {
            return; // 输满了,多的丢掉 —— 和 lock_input_digit() 同一个规矩
        }

        s_buf[s_len] = key;
        s_len++;

        // 只登记,不画 —— 见 admin.h 文件头。下面每一条 return 都必须保持
        // 这个性质,不然"按键路径不许碰 LCD"这条规矩就漏了。
        s_echo_msg = NULL;
        admin_set_redraw(ADMIN_REDRAW_ECHO);
        return;
    }

    if (key == '*')
    {
        printf("管理员:清空重输\r\n");
        s_len      = 0;
        s_echo_msg = NULL;
        admin_set_redraw(ADMIN_REDRAW_ECHO);
        return;
    }

    if (key == '#')
    {
        admin_submit();
        return;
    }

    // 其它键(理论上没有)直接忽略
}

/**
 * 🟡 L1 架构代码 —— 倒计时推进点:它必须由 lock_task 每 20ms 调一次,漏调的现象是"管理员模式进去就再也退不出来",而不是别的
 * @brief  推进管理员模式的倒计时 —— 由 lock_task 每 20ms 调一次
 * @note   三种状态用三个不同的时限:菜单用 15 秒的窗口,子模式(输密码)用 20 秒的
 *         宽松超时,结果提示只停 1.5 秒。
 * @note   倒计时算的是"最后一次按键之后过了多久",所以补画面花掉的那几百毫秒
 *         (菜单是等电机刹车后才开始画的)也是从窗口里扣的 —— 15 秒的窗口,
 *         用户实际能看清菜单再从容按键。
 * @note   算的是"现在离最后一次按键过了多久",用无符号差值 —— 和 lock_tick()
 *         算电机行程是同一个写法,能正确跨过 49.7 天的 tick 回绕。
 */
void admin_tick(void)
{
    uint32_t idle;
    uint32_t limit = ADMIN_SUB_TIMEOUT_MS;

    if (s_state == ADMIN_OFF)
    {
        return;
    }

    // ── ① 先把欠着的画面补上一点点(电机通着电就继续欠着)──
    admin_flush_redraw();

    // admin_flush_redraw() 抢不到屏时会把状态清成 ADMIN_OFF,这里必须重判一次:
    // 不然下面会拿着一个已经作废的状态去算倒计时,还要多打一句超时提示。
    if (s_state == ADMIN_OFF)
    {
        return;
    }

    // ── ② 再算"离最后一次按键过了多久" ──
    idle = xTaskGetTickCount() - s_last_key;

    switch (s_state)
    {
        case ADMIN_MENU:
            limit = ADMIN_WINDOW_MS;
            break;

        case ADMIN_MSG:
            limit = ADMIN_MSG_MS;
            break;

        default:
            limit = ADMIN_SUB_TIMEOUT_MS;
            break;
    }

    if (idle < limit)
    {
        return;
    }

    if (s_state == ADMIN_MENU)
    {
        printf("管理员模式:%d 秒没有输入,自动退出\r\n", ADMIN_WINDOW_MS / 1000);
    }
    else if (s_state != ADMIN_MSG)
    {
        printf("管理员:等输入超时,放弃本次操作\r\n");
    }
    // ADMIN_MSG 到点属于正常流程,不用打印

    admin_exit();
}
