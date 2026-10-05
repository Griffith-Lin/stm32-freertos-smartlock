#include "wifi.h"
#include "tim.h"        // GetTim10Tick():裸机计时用的 1ms 时基
#include "lock.h"       // lock_post_cmd():把云端下发翻译成锁控命令
#include "lcd.h"        // MAGENTA 等颜色宏(蓝牙配网时要往屏幕上打提示)
#include "wifi_cfg.h"   // WiFi 名/密码存在 EEPROM 里,开机读、配网后写
#include "FreeRTOS.h"
#include "task.h"
#include "stdio.h"
#include "string.h"

// ══════════════════════════════════════════════════════════════════════════
//  分包平台参数(改这里就能换平台,不用动逻辑)
//
//  下面这些是接入方给的接入信息,MQTT 的 Client ID 用 mqtt.fx 生成的那个。
//  ⚠ 密码是明文躺在源码里的 —— 教学项目无所谓,真做产品要么编译期加密,
//    要么出厂时写进 EEPROM 由用户自己配。
//
//  ⚠ WiFi 名和密码**已经不在这里了**。它们运行时存在 AT24C04 EEPROM 里
//    (见 wifi_cfg.c),开机读出来连网,蓝牙配网配好之后由固件自己写回去。
//    所以换网不用改代码、不用重新烧录,配一次网就够。
// ══════════════════════════════════════════════════════════════════════════

#define WIFI_MQTT_HOST      "sh-3-mqtt.iot-api.com"
#define WIFI_MQTT_PORT      "1883"
#define WIFI_MQTT_CLIENT_ID "9203454aeb1e4a4f94fd9423dac71221"
#define WIFI_MQTT_USER      "dwy5lftp43w54p7e"
#define WIFI_MQTT_PASS      "nE2s7UByXC"

#define WIFI_TOPIC_SUB      "attributes/push"   // 订阅:收云端下发
#define WIFI_TOPIC_PUB      "attributes"        // 发布:上报数据

// ── 云端下发消息(订阅到的消息)的行格式 ──
//    模块主动上报的一行长这样:
//
//        +EVENT:MQTT_SUB,attributes/push,21,{"Password":"123456"}
//        └──── 前缀 ────┘└── topic ────┘└len┘└──── payload ────┘
//
//    ⚠ len 是**平台声明的载荷字节数**,不是我们自己数的 —— 拿它和实际收到的
//      长度对账,就能发现"行缓冲太小、载荷被截断"这种静默错误。
//      见 wifi_handle_sub_event()。
//    ⚠ payload 里装的是开锁密码,回显一律要脱敏(见 wifi_print_masked())。
#define WIFI_EVT_SUB        "+EVENT:MQTT_SUB,"  // ⚠ 结尾的逗号算在常量里
#define WIFI_FIELD_PWD      "\"Password\""      // ⚠ 引号一起带上,免得误命中别的前缀
                                                //   (旧字段是 "DoorLock":1/0,已废弃)

// 收行缓冲大小。**payload 必须能整个装下**:被截断的 JSON 有可能还是能解析出
// 一个(错的)值,那比直接失败更危险。所以长了对不上账就整条丢弃。
#define WIFI_LINE_BUF       128

// ── 超时(ms)──
#define WIFI_BOOT_MS        2000    // 模块上电后等它就绪
#define WIFI_T_AT_MS        2000    // 普通 AT 命令
#define WIFI_T_CONFIG_MS    3000    // 逐条写 MQTT 参数
#define WIFI_T_WJAP_MS      25000   // 连 WiFi:可能要十几秒,给足
#define WIFI_T_CONN_MS      15000   // 连 MQTT / 订阅
#define WIFI_T_PUB_MS       5000    // 发一条消息

#define WIFI_PUB_PERIOD_MS  10000   // 心跳周期:10s 一条
#define WIFI_RETRY_MS       10000   // 联网失败后多久重试一次

// ── 蓝牙配网(blufi)──
//    手机装 esp BluFi App,连上模块开的热点,把 WiFi 名和密码发过去。
//    见手册 4.1.16 AT+WCONFIG。
#define WIFI_PROV_DEADLINE_MS  15000        // 开机多久还没连上 WiFi 就转蓝牙配网
#define WIFI_PROV_NAME         "DoorLock"   // 广播名,手机搜到的就是它
#define WIFI_T_PROV_MS         180000       // 开机那次自动配网的窗口:3 分钟
                                            // (管理员 555# 那次是 WIFI_PROV_ADMIN_MS)
#define WIFI_T_PROV_POLL_MS    2000         // 配网期间多久查一次状态
#define WIFI_PROV_SETTLE_MS    8000         // 窗口关闭后,再等多久判模块自己连上没有

// ── 管理员模式 555# 手动触发的配网 ──
//   开机那次自动配网给 3 分钟(用户可能不在旁边),手动这次只给 20 秒:
//   人是站在锁前面按的 555#,掏手机、开 App、连 BLE、填 SSID 全在这 20 秒里。
//   ⚠ 实测这个窗口很紧 —— 手机要先在系统设置里连上模块的 BLE 广播才能用
//     ESP BluFi App。嫌短就改这一个宏。
#define WIFI_PROV_ADMIN_MS     20000
// 收尾时结果在屏上停多久(和 admin.h 的 ADMIN_MSG_MS 同量级)
#define WIFI_PROV_MSG_MS       1500
// 中断上一次 AT 等待之后,等模块把那条命令的应答吐完再整批倒掉
#define WIFI_PROV_FLUSH_MS     100
// 可被打断的延时按这个粒度切片
#define WIFI_DELAY_SLICE_MS    50

// 一次 AT+WJAP 至少得给这么多时间,否则连"发出去 + 模块回话"都不够,
// 试了只会白发一条命令、留一个没人收的应答在缓冲里,把下一次读行搞乱。
#define WIFI_MIN_TRY_MS     1000

// ── 接收环形缓冲:ISR 往里放,任务从里取 ──
//    为什么不用 MG200.c 那种"发完命令再逐字节轮询收"的写法?
//    因为 WB2 会**主动**上报,而且时机不可预测:
//        +EVENT:MQTT_CONNECT / MQTT_DISCONNECT / MQTT_SUB,<topic>,<len>,<data>
//    这些事件可能在两条命令之间的任何时刻到达。轮询接收只在"正在等应答"
//    的时候才看串口,中间来的事件就永远丢了。所以这里必须中断收。
//
//    ⚠ USART2 只开中断接收,全工程**没有任何地方对它做轮询读** ——
//      ISR 会先把 RXNE 读走,轮询就永远等不到数据(CLAUDE.md 里记的坑)。
#define WIFI_RX_BUF_SIZE    256

static volatile uint8_t  s_rx_buf[WIFI_RX_BUF_SIZE];
static volatile uint16_t s_rx_head = 0;     // ISR 写指针
static volatile uint16_t s_rx_tail = 0;     // 任务读指针
static volatile uint32_t s_rx_drop = 0;     // 缓冲满被丢掉的字节数

// 最近一条"非 OK / 非 ERROR"的应答行。模块报错是**两行**:
//     AT+WJAP:66      ← 真正的原因在这行(手册 1.6 节有码表)
//     +ERROR
// 只看第二行等于只知道"失败了",不知道"为什么失败"。所以留一份给失败分支解读。
static char s_at_prev[64];

// ── "自动配网试过了吗" ──
//    触发条件是 "开机过了 15 秒 && 还没连上",而这个条件**过点之后永远为真**。
//    没有这个标志拦着,"用户没掏手机"这条最常见的路径就会:配网超时 → 下个循环
//    条件依然成立 → 又进配网,每次卡 3 分钟,无限循环。设备就一直停在"可被配网"
//    状态 —— 正好是这里最想避免的事。
//    所以它是"尝试过"的意思,**不是**"配成了"(成不成只看 wifi_is_wifi_up())。
static uint8_t s_prov_tried = 0;

// ── 管理员模式 555# 的手动配网请求 ──
//   admin 跑在 lock_task 里,配网跑在 wifi_task 里,所以这两个标志是跨任务的。
//   置位是单字节写(Cortex-M4 上原子),"取出并清零"那一步才要临界区 ——
//   见 wifi_take_prov_request()。
static volatile uint8_t s_prov_req = 0;     // 有一张待办的手动配网请求
static volatile uint8_t s_prov_run = 0;     // 正在配网中

/**
 * 🟡 L1 架构代码 —— "取出 + 清零"必须在同一个临界区:取出来之后、清零之前对方又置了一次,这次请求就被这次清零吃掉了;而这条原子性是"按下 555# 一定会被处理"的唯一保证
 * @brief  取出并清掉一张待办的手动配网请求
 * @return 1 = 有;0 = 没有
 */
static uint8_t wifi_take_prov_request(void)
{
    uint8_t req = 0;

    taskENTER_CRITICAL();
    if (s_prov_req)
    {
        s_prov_req = 0;
        req = 1;
    }
    taskEXIT_CRITICAL();

    return req;
}

// 🟢 L2 工具代码 —— 纯置位,没有副作用也没有先后要求;"投递意图而非执行"这条分工由调用方遵守
void wifi_request_provision(void)
{
    // 已经在配网了就别再置位。理由不是"省一次操作",而是 s_prov_req 在配网
    // 期间是**低层 AT 等待的打断开关**(见 wifi_get_byte)—— 配网中途被置位,
    // 最要紧的那条收尾命令 AT+WCONFIG=0 会被截断,模块就一直停在配网模式,
    // 等于长期开着"旁边谁都能把锁改到别的 WiFi 上去"的口子。
    if (s_prov_run)
    {
        printf("配网:已经在配网中,忽略这次请求\r\n");
        return;
    }

    s_prov_req = 1;
    printf("配网:收到手动配网请求(管理员模式 555#)\r\n");
}


/**
 * 🔴 L0 生死线代码 —— 中断里直接读 DR 清 RXNE,环形缓冲的 head/tail 是 ISR 和任务之间唯一的同步点;满了宁可丢新字节也不盖旧字节,盖一下就会把一整行 AT 应答从中间截断
 * @brief  USART2 接收中断:每来一个字节就塞进环形缓冲
 * @note   用 head==tail 判断空、用 head+1==tail 判断满 ——
 *         永远留一个空位,这样"空"和"满"不会混成同一种状态。
 *         满了就丢新字节并计数,而不是覆盖旧的 ——
 *         覆盖会把一行 AT 应答从中间截断,比丢整行更难查。
 */
void USART2_IRQHandler(void)
{
    if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET)
    {
        uint8_t  b    = (uint8_t)USART_ReceiveData(USART2); // 读 DR 顺带清 RXNE
        uint16_t next = (uint16_t)((s_rx_head + 1) % WIFI_RX_BUF_SIZE);

        if (next != s_rx_tail)
        {
            s_rx_buf[s_rx_head] = b;
            s_rx_head = next;
        }
        else
        {
            s_rx_drop++;
        }
    }
}


// ── 发送 ──

/**
 * 🟢 L2 工具代码 —— 契约就一条:轮询 TXE 再把字节塞进 DR;115200 下一个字节 87us,最坏几毫秒的忙等且只有优先级最低的 wifi_task 会走这里,不用管实时性
 * @brief  发一个字节(忙等 TXE)
 * @note   115200 下一个字节约 87us,整条 AT 命令也就几毫秒,
 *         而且只被 wifi_task(优先级 1,最低)调用,不会饿死业务任务。
 */
static void wifi_send_byte(uint8_t b)
{
    while (!USART_GetFlagStatus(USART2, USART_FLAG_TXE))
        ;
    USART_SendData(USART2, b);
}


/**
 * 🟢 L2 工具代码 —— 逐字节转发 + 补 CRLF,模块只认 "\r\n";唯一容易漏的是结尾要等 TC 而不是 TXE,否则最后一个字节可能在紧接着的操作里被截断
 * @brief  发一条 AT 命令,自动补 "\r\n"
 * @note   模块要求命令必须以 CRLF 结尾,只发 "\n" 不认。
 */
static void wifi_send_line(const char *s)
{
    while (*s != '\0')
    {
        wifi_send_byte((uint8_t)*s);
        s++;
    }
    wifi_send_byte('\r');
    wifi_send_byte('\n');

    // 等最后一个字节真正移出去(TC),否则紧接着的操作可能把它截断
    while (!USART_GetFlagStatus(USART2, USART_FLAG_TC))
        ;
}


// ── 接收 ──

/**
 * 🟡 L1 架构代码 —— 缓冲空的时候用 vTaskDelay(1) 让出 CPU 而不是忙等,这是"联网等几百毫秒很正常"换来的设计;同时它也划定了"只能在任务上下文调用"的边界,搬进 ISR 里就是等死
 * @brief  从环形缓冲取一个字节,取不到就等到超时
 * @param  out        输出:取到的字节
 * @param  start      本轮计时的起点,来自 GetTim10Tick()
 * @param  timeout_ms 从 start 起最多等多久
 * @return 1 = 取到了;0 = 到点还没有
 * @note   **只能在任务上下文调用** —— 缓冲空的时候用 vTaskDelay 让出 CPU。
 *         不用忙等:联网这种"等几百毫秒很正常"的场合,忙等会把
 *         同优先级和低优先级的任务全饿死。
 * @note   这里是**所有 AT 等待的唯一咽喉**,所以"手动配网请求要立刻生效"
 *         这件事也放在这儿:管理员按 555# 时,wifi_task 很可能正卡在一条
 *         超时 25 秒的 AT+WJAP 里等应答,不打断的话 20 秒的配网窗口
 *         等它醒过来就已经过去一大半了。返回 0 会被上层当成"超时",
 *         整条调用栈会一层层很快退回 wifi_task。
 *         ⚠ 打断会留下"没人收的应答",所以 wifi_task 处理请求前必须先
 *           wifi_prov_flush_rx() 倒一次,理由见那个函数。
 * @note   判断只看 s_prov_req,不看 s_prov_run:配网期间 s_prov_req 恒为 0
 *         (wifi_request_provision 会拒绝在配网中置位),所以配网自己发出去的
 *         命令不会被自己打断。
 */
static uint8_t wifi_get_byte(uint8_t *out, uint32_t start, uint32_t timeout_ms)
{
    while (s_rx_head == s_rx_tail)
    {
        if (s_prov_req)
        {
            return 0;
        }

        if ((GetTim10Tick() - start) >= timeout_ms)
        {
            return 0;
        }
        vTaskDelay(1);  // 让出 1ms,等 ISR 把数据填进来
    }

    *out = s_rx_buf[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1) % WIFI_RX_BUF_SIZE);
    return 1;
}


/**
 * 🟡 L1 架构代码 —— 超时是整行共享一个 deadline(不是每字节重新计时),行太长时只保留前段继续吞到行尾、绝不把一行劈成两行喂给上层;这两条策略决定了调用者拿到的行可不可信
 * @brief  取一整行(到 '\n' 为止),顺便去掉行尾的 '\r'
 * @param  line 输出缓冲
 * @param  max  缓冲大小(含 '\0')
 * @param  timeout_ms 超时
 * @return 行长(>=0);-1 = 超时没收全
 * @note   行太长时**只保留前面的字符继续吞到行尾**,不越界也不截断成两行。
 */
static int wifi_read_line(char *line, uint16_t max, uint32_t timeout_ms)
{
    uint32_t start = GetTim10Tick();
    uint16_t n = 0;
    uint8_t  b;

    for (;;)
    {
        if (!wifi_get_byte(&b, start, timeout_ms))
        {
            line[n] = '\0';
            return -1;
        }

        if (b == (uint8_t)'\n')
        {
            if ((n > 0) && (line[n - 1] == '\r'))
            {
                n--;                    // 去掉 "\r\n" 里的 '\r'
            }
            line[n] = '\0';
            return (int)n;
        }

        if (n < (uint16_t)(max - 1))
        {
            line[n++] = (char)b;
        }
    }
}


/**
 * 🟡 L1 架构代码 —— 日志脱敏的唯一出口:云端下发的载荷里装的就是开锁密码,一旦原样进串口就等于把密码写进了会被截图外发的日志,所有回显都得过这一道
 * @brief  把文本里的数字全部打成 '*' 之后打印(不换行)
 * @param  text 要打印的文本
 * @note   为什么要这么干:USART1 那头接的是电脑,调试日志经常被截图或复制
 *         外发。解析失败的原文更是五花八门(可能截断、可能夹着别的东西),
 *         与其逐个判断"这段里有没有密码",不如统一"数字一律不打印"。
 * @note   数字以外的字符(字段名、引号、冒号、大括号)照原样打 —— 那些正是
 *         排查"云端到底发了个什么结构"要看的东西,而密码本身是纯数字。
 * @note   先拼进中间缓冲再一次 printf,而不是逐字符 putchar:fputc 每调一次
 *         就进一次 USART1,拼一下能省好几倍的函数调用。
 * @note   缓冲大小按 WIFI_LINE_BUF 给:调用方传进来的都是这个尺寸的行缓冲
 *         (wifi_session / wifi_handle_sub_event 里的 line),绝不会更长。
 */
static void wifi_print_masked(const char *text)
{
    char    masked[WIFI_LINE_BUF];
    uint8_t i = 0;

    while ((text[i] != '\0') && (i < (WIFI_LINE_BUF - 1)))
    {
        masked[i] = ((text[i] >= '0') && (text[i] <= '9')) ? '*' : text[i];
        i++;
    }
    masked[i] = '\0';

    printf("%s", masked);
}

/**
 * 🟡 L1 架构代码 —— AT 收发的中心:超时从发命令那一刻算总账、用子串而不是 strcmp 匹配 "ERROR" 以兜住模块真回的 "+ERROR"、非 OK/ERROR 的行存进 s_at_prev 供失败分支解读
 * @brief  发一条 AT 命令,逐行收应答,直到 OK / ERROR
 * @param  cmd        命令正文,不含结尾的 CRLF
 * @param  watch      想额外捞出来的关键字,NULL 表示不捞
 * @param  cap        捞到的行写这里(可为 NULL)
 * @param  cap_size   cap 的字节数
 * @param  timeout_ms 总共允许等多久(不是每行都重新计时)
 * @return 1 = 收到 OK;0 = 收到 ERROR 或超时
 * @note   中间模块说的每一行都会原样转发到 USART1 控制台,前缀 <。
 *         调试阶段这一步的价值最大 —— 模块到底回了什么,一眼就能看见,
 *         不用拿逻辑分析仪去猜。
 * @note   "+EVENT:xxx" 这类主动上报也会被顺手打印出来,然后继续等 OK。
 *         ⚠ 但 +EVENT:MQTT_SUB(云端下发)里装着开锁密码,那一行走
 *         wifi_print_masked() 脱敏 —— 见下面的说明。
 */
static uint8_t wifi_at_ex(const char *cmd, const char *watch,
                          char *cap, uint16_t cap_size, uint32_t timeout_ms)
{
    char     line[128];
    uint32_t start = GetTim10Tick();
    int      n;

    if ((cap != NULL) && (cap_size > 0))
    {
        cap[0] = '\0';
    }

    printf("AT> %s\r\n", cmd);
    wifi_send_line(cmd);

    for (;;)
    {
        uint32_t elapsed = GetTim10Tick() - start;

        // 管理员按了 555#:当场放弃这条命令。放在这个位置(而不是只写在
        // wifi_get_byte 里)是因为 wifi_get_byte 一返回 0,下面那句 continue
        // 会立刻转回来 —— 没有这一句就是空转烧 CPU,烧到 timeout_ms 为止,
        // 而 WJAP 的超时是 25 秒。
        if (s_prov_req)
        {
            printf("AT< [手动配网请求,中断本条命令的等待]\r\n");
            return 0;
        }

        if (elapsed >= timeout_ms)
        {
            printf("AT< [超时] 等了 %ums 没等到 OK/ERROR\r\n", (unsigned)timeout_ms);
            return 0;
        }

        n = wifi_read_line(line, sizeof(line), timeout_ms - elapsed);
        if (n < 0)
        {
            continue;   // 上面那次判断会兜住,这里只是把超时收敛到同一个出口
        }

        if (n > 0)
        {
            // ⚠ 云端下发的消息会以 +EVENT:MQTT_SUB 的形式**插进任何一条 AT
            //   命令的等待里**(模块是主动上报的,不挑时候),而那条里装着
            //   开锁密码,不能原样进日志。别的 AT 应答里没有密码,照原样打
            //   才好查问题 —— 所以只对这一个前缀脱敏,不是一律打成星号。
            if (strncmp(line, WIFI_EVT_SUB, sizeof(WIFI_EVT_SUB) - 1) == 0)
            {
                printf("AT< ");
                wifi_print_masked(line);
                printf("\r\n");
            }
            else
            {
                printf("AT< %s\r\n", line);
            }

            if ((strcmp(line, "OK") != 0) && (strcmp(line, "ERROR") != 0))
            {
                strncpy(s_at_prev, line, sizeof(s_at_prev) - 1);
                s_at_prev[sizeof(s_at_prev) - 1] = '\0';
            }
        }

        if ((watch != NULL) && (strstr(line, watch) != NULL) &&
            (cap != NULL) && (cap_size > 0))
        {
            strncpy(cap, line, cap_size - 1);
            cap[cap_size - 1] = '\0';
        }

        if (strcmp(line, "OK") == 0)
        {
            return 1;
        }

        // ⚠ 坑:模块报错回的是 "+ERROR"(**带加号**),不是 "ERROR"。
        //   用 strcmp 精确比会漏判,命令就一直傻等到超时才失败 ——
        //   WJAP 那种 25s 超时的,等于每次白等 25 秒。所以用包含匹配。
        //   唯一副作用:主动上报里的 "+EVENT:MQTT_MALLOC_ERROR" 也会命中,
        //   但那个本来就是真错误,提前返回反而是对的。
        if (strstr(line, "ERROR") != NULL)
        {
            return 0;
        }
        // 其它行(+EVENT…、回显…)打印完继续等
    }
}


/**
 * 🟢 L2 工具代码 —— 一行转发,契约只有"回了 OK 就算成";超时怎么算、+ERROR 怎么认、事件行怎么留,坑全在 wifi_at_ex 里,这里改不出花样
 * @brief  wifi_at_ex 的简化版:不关心应答内容,只想知道成没成
 */
static uint8_t wifi_at_cmd(const char *cmd, uint32_t timeout_ms)
{
    return wifi_at_ex(cmd, NULL, NULL, 0, timeout_ms);
}


/**
 * 🟡 L1 架构代码 —— "命令回 OK ≠ 事情办成"这条 AT 模块的通用语义就落在这里:连 MQTT 必须等到 +EVENT:MQTT_CONNECT 才敢走下一步,只等 OK 就去干别的,一连云端必翻车
 * @brief  等模块主动上报某个事件
 * @param  token      关键字,比如 "+EVENT:MQTT_CONNECT"
 * @param  timeout_ms 最多等多久
 * @return 1 = 等到了;0 = 超时
 * @note   **命令回的 OK 只代表"命令收到了",不代表"事情办成了"。**
 *         AT 模块普遍是这个语义,连 MQTT 尤其明显:
 *             AT+MQTT        → 立刻回 OK
 *             +EVENT:MQTT_CONNECT   ← 过一会儿才来,这才叫连上了
 *         所以凡是"要等结果"的步骤都得等事件;只等 OK 就去干下一步,
 *         中间那段空档在大局域网里看不出来,一连云端 broker 就现形。
 */
static uint8_t wifi_wait_event(const char *token, uint32_t timeout_ms)
{
    char     line[128];
    uint32_t start = GetTim10Tick();
    int      n;

    for (;;)
    {
        uint32_t elapsed = GetTim10Tick() - start;

        // 和 wifi_at_ex 里那一句同理:这里是等 +EVENT 的地方,
        // 而 +EVENT:MQTT_CONNECT 的超时是 15 秒,不打断就白等。
        if (s_prov_req)
        {
            printf("AT< [手动配网请求,中断等待 %s]\r\n", token);
            return 0;
        }

        if (elapsed >= timeout_ms)
        {
            return 0;
        }

        n = wifi_read_line(line, sizeof(line), timeout_ms - elapsed);
        if (n > 0)
        {
            printf("AT< %s\r\n", line);
            if (strstr(line, token) != NULL)
            {
                return 1;
            }
        }
    }
}


/**
 * 🟡 L1 架构代码 —— 只认 status 3(连上且拿到 IP)才算通,1/2 都当没通;这个判据同时决定"要不要重发 WJAP"和"蓝牙配网算不算成功",松一格就会出现"看着连上了其实没网"
 * @brief  问模块"WiFi 现在是通的吗"
 * @return 1 = 已连上且拿到 IP;0 = 没通(或查询本身失败)
 * @note   用 AT+WJAP? 查,回的是:
 *             +WJAP:<status>,<ssid>,<pwd>,<bssid>,<加密>,<MAC>,<ch>,<IP>,<网关>
 *         status 含义:0=没连 1=正在连 2=连上但没 IP 3=连上且有 IP 4=连失败
 *         只认 3。后面 8 个字段不解析 —— 用不到,解析了反而是维护负担。
 */
static uint8_t wifi_is_wifi_up(void)
{
    char        cap[128];
    const char *p;

    if (!wifi_at_ex("AT+WJAP?", "+WJAP:", cap, sizeof(cap), WIFI_T_AT_MS))
    {
        return 0;
    }

    p = strchr(cap, ':');
    if (p == NULL)
    {
        return 0;
    }

    p++;
    while (*p == ' ')
    {
        p++;    // 手册排版里冒号后面有空格,实际串口输出不一定有,两种都得认
    }

    return (uint8_t)(*p == '3');
}


/**
 * 🟡 L1 架构代码 —— 每次现算而不是进循环前算一次:探活 + AT+WMODE 本身就要花掉几秒,拿旧值会让这次连接透支掉配网窗口;下界兜到 WIFI_MIN_TRY_MS 是为了不发那种"发出去也没人收"的白命令
 * @brief  算"开机 15 秒配网窗口"里还剩多少毫秒可以拿来连 WiFi
 * @param  boot 开机时刻(GetTim10Tick() 取的)
 * @return 剩余毫秒,最少给 WIFI_MIN_TRY_MS
 * @note   为什么每次都要**现算**,不能进循环前算一次存着:
 *         AT 探活 + AT+WMODE 这两步本身就可能花掉好几秒,拿循环开头那个值
 *         会让这次连接多透支那几秒,配网就开晚了 —— "开机 15 秒"的承诺就破了。
 * @note   下界兜到 WIFI_MIN_TRY_MS:算出来 0 的话连"发出去 + 模块回话"都不够,
 *         白发一条命令还会留个没人收的应答在缓冲里,把下一次读行搞乱。
 * @note   用无符号差值写,自动跨过 49.7 天的回绕。
 */
static uint32_t wifi_join_budget(uint32_t boot)
{
    uint32_t used = GetTim10Tick() - boot;
    uint32_t left = (used >= WIFI_PROV_DEADLINE_MS) ? 0 : (WIFI_PROV_DEADLINE_MS - used);

    return (left < WIFI_MIN_TRY_MS) ? WIFI_MIN_TRY_MS : left;
}


/**
 * 🟡 L1 架构代码 —— 发 WJAP 之前先查一次 AT+WJAP?,是因为有些固件在"已连接"状态下重发会报错,那样会把好端端的连接误判成失败,陷入"明明能用却一直重连"的死循环
 * @brief  连 WiFi:已经连着就不动,没连才发 AT+WJAP
 * @param  ssid       WiFi 名(从 EEPROM 读出来的)
 * @param  pwd        密码
 * @param  timeout_ms 这次最多等多久
 * @return 1 = WiFi 通了;0 = 没通(模块的回话已打到控制台)
 * @note   为什么要先查一次 AT+WJAP? 而不是每次都直接连?
 *         有些固件在"已连接"状态下重发 WJAP 会报错,那样就把好好的连接
 *         误判成失败,陷入"明明能用却一直重连"的死循环。
 * @note   发命令的缓冲够长就行:AT+WJAP= 是 8 个字符,加上 SSID 32 + 逗号 +
 *         密码 63,一共 104 字节,下面留了 111。
 */
static uint8_t wifi_wifi_join(const char *ssid, const char *pwd, uint32_t timeout_ms)
{
    char cmd[16 + WIFI_CFG_SSID_MAX + WIFI_CFG_PWD_MAX];

    if (wifi_is_wifi_up())
    {
        printf("---- WiFi 已经连着,不用再连 ----\r\n");
        return 1;
    }

    sprintf(cmd, "AT+WJAP=%s,%s", ssid, pwd);

    printf("---- 连接 WiFi: [%s] ----\r\n", ssid);
    if (wifi_at_cmd(cmd, timeout_ms))
    {
        printf("---- WiFi 连上了 ----\r\n");
        return 1;
    }

    // 失败时别只说"失败了" —— 模块其实把原因告诉我们了,只是藏在一个数字里。
    // 报错行的格式是 "AT+WJAP:<错误码>",码表在手册 1.6 节
    // (最常撞上的:66 扫到了但连不上 / 69 根本没扫到这个名字 / 74 STA 没在跑)。
    printf("!! 连接失败,模块的回话:%s(码表见手册 1.6 节)\r\n", s_at_prev);
    return 0;
}


/**
 * 🟡 L1 架构代码 —— 配网成功后唯一一处"把用户发给模块的凭据从它手里抄回来"的地方,不存则下次上电就得重配;字段按逗号切分,所以要提防 SSID/密码里带逗号时切错位
 * @brief  把模块当前连着的这个 WiFi 的名字和密码抄下来,存进 EEPROM
 * @return 1 = 存好了;0 = 没问到 / 内容不对,没存
 * @note   **蓝牙配网成功后必须调这个**,否则用户配的网下次上电就没了 ——
 *         配网期间用户是用手机把凭据发给**模块**的,固件手里一个字都没有。
 * @note   凭据从哪来:AT+WJAP? 的应答**明文带着 SSID 和密码**:
 *             +WJAP:3,<ssid>,<pwd>,<bssid>,<加密>,<MAC>,<信道>,<IP>,<网关>
 *         status 3 = 连上且拿到 IP,这时模块手里的凭据一定是可用的,
 *         抄下来存进 EEPROM 比让用户再报一遍靠谱得多。
 * @note   ⚠ 局限:SSID 或密码里**带逗号**的话会切错字段(协议就是这么定的,
 *         AT 命令一律用逗号分隔参数)。家用场景基本不会遇到;
 *         真遇到了就在 wifi_cfg_save() 里加一层长度校验挡住。
 * @note   名字最长 32、密码最长 63,`+WJAP:3,` 8 个字符,加起来 104 ——
 *         不会超出 wifi_at_ex() 里那个 128 字节的行缓冲,拿得到完整的行。
 */
static uint8_t wifi_ap_creds_save(void)
{
    char        cap[160];
    char        ssid[WIFI_CFG_SSID_MAX + 1];
    char        pwd[WIFI_CFG_PWD_MAX + 1];
    const char *p;
    uint16_t    i;

    if (!wifi_at_ex("AT+WJAP?", "+WJAP:", cap, sizeof(cap), WIFI_T_AT_MS))
    {
        return 0;
    }

    p = strchr(cap, ':');
    if (p == NULL)
    {
        return 0;
    }
    p++;

    // ── 跳过 status 字段(手册排版里冒号后面可能有空格,两种都得认)──
    while ((*p != '\0') && (*p != ','))
    {
        p++;
    }
    if (*p != ',')
    {
        return 0;
    }
    p++;

    // ── 第 2 个字段:SSID ──
    i = 0;
    while ((*p != '\0') && (*p != ',') && (i < WIFI_CFG_SSID_MAX))
    {
        ssid[i] = *p;
        i++;
        p++;
    }
    ssid[i] = '\0';

    if ((*p != ',') || (i == 0))
    {
        return 0;   // 没有下一个字段(说明压根没连上),或者名字是空的
    }
    p++;

    // ── 第 3 个字段:密码 ──
    i = 0;
    while ((*p != '\0') && (*p != ',') && (i < WIFI_CFG_PWD_MAX))
    {
        pwd[i] = *p;
        i++;
        p++;
    }
    pwd[i] = '\0';

    return wifi_cfg_save(ssid, pwd);
}


/**
 * 🟡 L1 架构代码 —— 先查再订是必须的:订阅列表记在模块自己的 flash 里、重连后它还会自己重订,重复订阅回的 199 手册里却写作 "unsubscribe failed",照着码表查会一路查到沟里去
 * @brief  查模块的订阅列表里有没有我们那个话题
 * @return 1 = 在册(状态 2/3);0 = 不在,或查询本身失败
 * @note   **为什么订阅之前一定要先查?**
 *         话题列表是记在**模块自己**身上的,掉电才清;而且手册写着重连后
 *         会自动重订(状态 2 就是"重连后重新订阅中")。所以第二次上电跑
 *         这段代码时,列表里往往已经有一份了 —— 这时再发 AT+MQTTSUB 就是
 *         重复订阅,固件会回错误,而且报的是 **199**,手册里写作
 *         "MQTT unsubscribe failed",和我们正在做的 subscribe 八竿子打不着,
 *         照着错误码查会一路查到沟里去。
 *         教训:AT 模块的错误码是"这个模块这次操作没成",不是"你哪里写错了",
 *         含义对不上时要以**当前状态**为准 —— 所以先查状态。
 */
static uint8_t wifi_is_subscribed(void)
{
    char        cap[128];
    const char *p;

    // 直接把话题名当关键字捞:应答形如 "<status>,<topic>" 一行一个话题,
    // 捞到的那行开头就是状态值。命令回显 "AT+MQTTSUB?" 不含话题名,不会误捞。
    if (!wifi_at_ex("AT+MQTTSUB?", WIFI_TOPIC_SUB, cap, sizeof(cap), WIFI_T_AT_MS))
    {
        return 0;
    }

    if (cap[0] == '\0')
    {
        return 0;   // 列表里没这个话题
    }

    // status: 0=初始 1=首次订阅中 2=重连后重订中 3=订阅成功
    // 2 和 3 都算"在册" —— 2 是正在重订,等它订完就行,不用我们插手。
    p = cap;
    while (*p == ' ')
    {
        p++;
    }

    printf("   (订阅列表里已有:%s)\r\n", cap);

    return (uint8_t)((*p == '2') || (*p == '3'));
}


/**
 * 🟡 L1 架构代码 —— 只认 status 3;这一个判断守着 AT+MQTT "已连接时再执行会强制断开重连"的行为,判错就等于每 10 秒把好端端的连接掐断重建一次,还顺带骚扰 broker
 * @brief  问模块"MQTT 现在是连着的吗"
 * @return 1 = 已连接(status 3);0 = 没连上,或查询失败
 * @note   手册:AT+MQTT? 回 "+MQTT:<status>,<主机>,<端口>,<方式>,<ClientID>,…"
 *         status: 0=初始 1=连接中 2=订阅中 3=连接成功
 */
static uint8_t wifi_is_mqtt_up(void)
{
    char        cap[128];
    const char *p;

    if (!wifi_at_ex("AT+MQTT?", "+MQTT:", cap, sizeof(cap), WIFI_T_AT_MS))
    {
        return 0;
    }

    p = strchr(cap, ':');
    if (p == NULL)
    {
        return 0;
    }

    p++;
    while (*p == ' ')
    {
        p++;
    }

    return (uint8_t)(*p == '3');
}


/**
 * 🟡 L1 架构代码 —— 重复订阅回的是 199 这种字面意思完全对不上的错误码,所以这里的原则是"先看状态再决定发不发命令";订不上时还把当前列表整个打出来,把"其实已有"和"真订不上"当场分开
 * @brief  确保云端下发的话题在订阅列表里
 * @return 1 = 在册(原本就在,或刚订上);0 = 订不上
 */
static uint8_t wifi_ensure_subscribed(void)
{
    // 已经订过就别再订一次(重连后模块会自己重订,列表是持久的)——
    // 重复订阅会回 199,那个码值的字面意思又完全对不上,极难查。
    if (wifi_is_subscribed())
    {
        printf("---- 订阅已在册,跳过 ----\r\n");
        return 1;
    }

    printf("---- 订阅 %s ----\r\n", WIFI_TOPIC_SUB);
    if (!wifi_at_cmd("AT+MQTTSUB=" WIFI_TOPIC_SUB ",0", WIFI_T_CONN_MS))
    {
        // 订不上时把当前列表整个打出来:是"其实已经有了"还是"真订不上",
        // 看一眼列表就知道了,不用靠猜。
        printf("!! 订阅失败(错误码见上一行),当前订阅列表是:\r\n");
        wifi_at_cmd("AT+MQTTSUB?", WIFI_T_AT_MS);
        return 0;
    }

    return 1;
}


/**
 * 🟡 L1 架构代码 —— 七条 AT+MQTT= 配置有先后依赖、发完 AT+MQTT 还要再等 +EVENT:MQTT_CONNECT 才算真连上;开头"已连接就整段跳过"是学乖的结果——以前每轮重试都重发,等于自己周期性把连接掐断
 * @brief  配置 MQTT 参数 → 连接 → 订阅
 * @return 1 = 订阅成功;0 = 中途失败
 * @note   手册里这几项是分开设的(AT+MQTT=<key>,<data>),必须一条条发:
 *           1=域名 2=端口 3=连接方式 4=ClientID 5=用户名 6=密码 7=遗嘱
 *         连接方式选 1(TCP):平台给的端口是 1883,是明文 MQTT;
 *         8883 才是 TLS,那种要走 AT+MQTT=3,2 并额外烧证书。
 */
static uint8_t wifi_mqtt_connect(void)
{
    // 已经连着就**别碰它**。手册原话:
    //   "If the current MQTT task has been started, it will reconnect
    //    if it is executed again"
    // 也就是 AT+MQTT 在已连接时会强制断开重连 —— 原来这里每次重试都无条件
    // 重发一遍,等于每 10 秒把好端端的连接掐断重建一次(外加 7 条配置命令)。
    // 现象就是"一直不停的连上又断开",而且模块被反复逼着做最耗资源的
    // "拆了重建",还顺带把 broker 骚扰个没完。
    // 教训:凡是"确保 X 就绪"的函数,第一句都该是先查 X 在不在 —— 不为省时间,
    //       而是因为**很多 AT 命令在"已就绪"状态下执行会起反作用**。
    if (wifi_is_mqtt_up())
    {
        printf("---- MQTT 已连接,跳过配置和连接 ----\r\n");
        if (!wifi_ensure_subscribed())
        {
            return 0;
        }
        printf("==== MQTT 就绪 ====\r\n");
        return 1;
    }

    printf("---- 配置 MQTT ----\r\n");

    if (!wifi_at_cmd("AT+MQTT=1," WIFI_MQTT_HOST, WIFI_T_CONFIG_MS))      return 0;
    if (!wifi_at_cmd("AT+MQTT=2," WIFI_MQTT_PORT, WIFI_T_CONFIG_MS))      return 0;
    if (!wifi_at_cmd("AT+MQTT=3,1", WIFI_T_CONFIG_MS))                    return 0;
    if (!wifi_at_cmd("AT+MQTT=4," WIFI_MQTT_CLIENT_ID, WIFI_T_CONFIG_MS)) return 0;
    if (!wifi_at_cmd("AT+MQTT=5," WIFI_MQTT_USER, WIFI_T_CONFIG_MS))      return 0;
    if (!wifi_at_cmd("AT+MQTT=6," WIFI_MQTT_PASS, WIFI_T_CONFIG_MS))      return 0;

    // 遗嘱消息:这里不需要,话题给空串关掉。
    // 本应该"设备掉线时让平台自动知道",做产品时值得配上 ——
    // 话题填 attributes、payload 填 {"online":false} 之类。
    if (!wifi_at_cmd("AT+MQTT=7,\"\",0,0,\"\"", WIFI_T_CONFIG_MS))        return 0;

    printf("---- 连接 MQTT ----\r\n");
    if (!wifi_at_cmd("AT+MQTT", WIFI_T_CONN_MS))                          return 0;

    // 等到 +EVENT:MQTT_CONNECT 才算真连上。这一步失败,多半是
    // 主机/端口/用户名/密码/ClientID 有一项和平台对不上 ——
    // 平台不会告诉你哪一项错,只会拒绝,所以四项一起核对。
    if (!wifi_wait_event("+EVENT:MQTT_CONNECT", WIFI_T_CONN_MS))
    {
        printf("!! 没等到 MQTT_CONNECT,核对主机/端口/用户名/密码/ClientID\r\n");
        return 0;
    }

    if (!wifi_ensure_subscribed())
    {
        return 0;
    }

    printf("==== MQTT 就绪 ====\r\n");
    return 1;
}


/**
 * 🔴 L0 生死线代码 —— 这是"云端一段字符串"变成"物理开锁动作"的唯一转换点,解析猜错一个字符就是误开锁;所以认不出字段、位数不对、混了非数字、载荷像被截断时都直接丢弃,绝不瞎猜
 * @brief  把云端下发的 JSON 载荷翻译成开锁意图,投给 lock_task
 * @param  payload 形如 {"Password":"123456"} 的文本
 * @note   平台约定(定义在 lock.h):
 *             {"Password":"123456"}  → 6 位密码和锁密码一致才开锁
 *         比对走的是 lock_check_password(),**和本地键盘输入同一份实现** ——
 *         不存在"云端另有一套密码"这回事。管理员用 222# 改了密码,云端跟着
 *         用新的就行,两边天然一致。
 * @note   ⚠ **本函数以及整条上报路径都不许原样打印 payload/line**:里面装的
 *         就是开锁密码,而 USART1 那头接着电脑、日志会被截图外发。所有回显
 *         一律走 wifi_print_masked() 把数字打成 '*'。
 * @note   为什么不上 cJSON? 载荷就一个字段,为它拉一个 JSON 库进来
 *         (还有 malloc 和栈的代价)不划算。但**这种手写解析只在载荷形状
 *         完全可控时才成立** —— 一旦云端改字段名、载荷里出现嵌套对象,
 *         或者要处理字符串转义,就得换成真正的 JSON 解析器。
 * @note   这里是"尽力而为"的解析:认不出来就放弃,**绝不瞎猜**。
 *         猜错的代价是误开锁。
 */
static void wifi_handle_downlink(const char *payload)
{
    const char *p;
    uint8_t     pwd[LOCK_PWD_LEN];
    uint8_t     n;

    // 找 "Password"(引号一起找,免得将来云端多个 "PasswordExpire"
    // 之类的字段时被误命中)
    p = strstr(payload, WIFI_FIELD_PWD);
    if (p == NULL)
    {
        printf("!! 载荷里没有 Password 字段,忽略: ");
        wifi_print_masked(payload);
        printf("\r\n");
        return;
    }

    p += sizeof(WIFI_FIELD_PWD) - 1;    // 跳过 "Password"

    // 跳过冒号和空白:{"Password" : "123456"} 这种带空格的写法也要认。
    // 判空是为了防着载荷被截断成 {"Password" 这种半截样子时越界读。
    while ((*p == ' ') || (*p == '\t') || (*p == ':'))
    {
        p++;
    }

    // 值必须是**带引号的字符串**。这里故意不认裸数字 {"Password":123456}:
    // 数字形式的 012345 连前导零都保不住,与其猜,不如拒掉让对方改对。
    if (*p != '"')
    {
        printf("!! Password 的值不是字符串(少了引号),忽略\r\n");
        return;
    }
    p++;

    // ── 收密码:必须**恰好** LOCK_PWD_LEN 位数字 ──
    //   逐位查,遇到非数字(包括串提前结束的 '\0')就整条丢弃。
    for (n = 0; n < LOCK_PWD_LEN; n++)
    {
        if ((p[n] < '0') || (p[n] > '9'))
        {
            printf("!! Password 不是 %d 位纯数字,忽略\r\n", LOCK_PWD_LEN);
            return;
        }
        pwd[n] = (uint8_t)p[n];
    }

    // ⚠ 这一句不能省:少了它,{"Password":"12345678"} 会被当成 123456 收下
    //   —— 多出来的位数被静默无视,反而让"长一位的密码"也开得了门。
    //   (能安全读到 p[LOCK_PWD_LEN] 是因为上面六位已经保证是数字、不是串尾。)
    if (p[LOCK_PWD_LEN] != '"')
    {
        printf("!! Password 不止 %d 位,忽略\r\n", LOCK_PWD_LEN);
        return;
    }

    // ── 比对:和本地键盘输入共用 lock_check_password() ──
    //   ⚠ 失败分支也不许把 pwd 打出来,只能说"不对"。
    if (!lock_check_password(pwd, LOCK_PWD_LEN))
    {
        printf("云端下发:密码不对,拒绝\r\n");
        return;
    }

    printf("云端下发:密码正确,开锁\r\n");
    lock_post_cmd(LOCK_CMD_OPEN);
}


/**
 * 🟡 L1 架构代码 —— 只能按前两个逗号切分:payload 是 JSON,天生带逗号,无脑 strtok 会把载荷切碎;还要拿平台声明的 len 和实际收到的长度对账,免得被截断的 JSON 解析出一个错的值
 * @brief  处理一行 +EVENT:MQTT_SUB 上报(云端有消息下来了)
 * @param  line 完整一行,形如:
 *              +EVENT:MQTT_SUB,attributes/push,21,{"Password":"123456"}
 * @note   ⚠ **这条行里装着开锁密码**,所以本函数所有的回显都走
 *         wifi_print_masked() 把数字打成 '*' —— 报错也不能把原文吐出去。
 *         详见 wifi_handle_downlink() 的说明。
 * @note   ⚠ **只能按"前两个逗号"切分**。payload 是 JSON,里面天生带逗号
 *         (比如 {"a":1,"b":2}),要是无脑 strtok 把整行按逗号全劈开,
 *         载荷就被切成好几段了。这是这类 AT 模块最常见的翻车点。
 * @note   ⚠ 第三个字段 len 是**平台声明的**载荷字节数,不是我们数的。
 *         拿它和实际收到的长度对账,就能抓到"wifi_read_line 缓冲太小
 *         把载荷截断了"—— 截断的 JSON 有可能还能解析出一个错的值,必须丢弃。
 */
static void wifi_handle_sub_event(const char *line)
{
    const char *topic;
    const char *len_str;
    const char *payload;
    const char *comma;
    uint32_t    want_len = 0;
    uint32_t    got_len;
    uint32_t    topic_len;
    uint32_t    i;

    // 跳过 "+EVENT:MQTT_SUB,"
    topic = line + (sizeof(WIFI_EVT_SUB) - 1);

    // ── 第 1 个逗号:topic 到此为止 ──
    comma = strchr(topic, ',');
    if (comma == NULL)
    {
        printf("!! 上报格式不对(少了长度字段),整行(数字已脱敏): ");
        wifi_print_masked(line);
        printf("\r\n");
        return;
    }
    topic_len = (uint32_t)(comma - topic);

    // 只处理我们订阅的那个话题。别的话题说明模块里的订阅配置和预期不一致
    // (多半是模块里还留着上一次跑留下的订阅),打印出来而不是闷头处理。
    if ((topic_len != strlen(WIFI_TOPIC_SUB)) ||
        (strncmp(topic, WIFI_TOPIC_SUB, topic_len) != 0))
    {
        // 这个话题名本身不含凭据,照原样打 —— 而且打的必须是**实际收到的**
        // 那个,打印我们期望的 WIFI_TOPIC_SUB 常量对排查毫无帮助。
        // (原来打的就是常量,等于每次都在说"我期望的是这个",白打。)
        // 正文一概不碰:它不属于我们这条链路,里面是什么都不该进日志。
        printf("!! 收到非本端订阅话题的消息,忽略: 话题=");
        for (i = 0; i < topic_len; i++)
        {
            putchar(topic[i]);
        }
        printf("\r\n");
        return;
    }

    // ── 第 2 个逗号:长度字段到此为止,后面全是 payload ──
    len_str = comma + 1;
    comma   = strchr(len_str, ',');
    if (comma == NULL)
    {
        printf("!! 上报格式不对(少了载荷),整行(数字已脱敏): ");
        wifi_print_masked(line);
        printf("\r\n");
        return;
    }
    payload = comma + 1;

    // ── 长度字段转数字 ──
    // 不引 atoi:它遇到非数字的行为是实现定义的,这里只认纯数字,自己判更稳。
    for (; len_str < comma; len_str++)
    {
        if ((*len_str < '0') || (*len_str > '9'))
        {
            printf("!! 长度字段不是数字,整条丢弃(数字已脱敏): ");
            wifi_print_masked(line);
            printf("\r\n");
            return;
        }
        want_len = (want_len * 10) + (uint32_t)(*len_str - '0');
    }

    got_len = (uint32_t)strlen(payload);

    printf("云端消息:话题=%s,%u 字节,载荷=", WIFI_TOPIC_SUB, got_len);
    wifi_print_masked(payload);
    printf("\r\n");

    if (got_len != want_len)
    {
        printf("!! 载荷长度对不上(平台声明 %u,实际收到 %u)"
               " —— 多半是收行缓冲 %d 字节太小把载荷截断了,"
               "内容已不可信,整条丢弃\r\n",
               want_len, got_len, WIFI_LINE_BUF);
        return;
    }

    wifi_handle_downlink(payload);
}


/**
 * 🟡 L1 架构代码 —— 常驻收发循环的调度策略:有主动上报就先处理事件(别急着发心跳),收到 MQTT_DISCONNECT 立刻返回交给上层,而不是自己硬撑——只重连不重订阅会"连上了却收不到消息"
 * @brief  MQTT 就绪之后的常驻循环:转发上报 + 定时发心跳
 * @note   两个返回条件:MQTT 掉线(交给上层重连),或者管理员按了 555#
 *         (交给 wifi_task 那个循环去配网)。
 */
static void wifi_session(void)
{
    char     cmd[160];
    char     line[WIFI_LINE_BUF];
    uint32_t last_pub = GetTim10Tick();
    int      hb = 0;
    int      n;

    for (;;)
    {
        // 管理员按了 555# —— 本任务正卡在这个永不退出的循环里,
        // 不主动看一眼的话请求永远没人受理。真正的配网在 wifi_task
        // 的循环顶上做(那里才有 AT 状态机的上下文)。
        if (s_prov_req)
        {
            printf("!! 管理员要求手动配网,退出常驻收发\r\n");
            return;
        }

        // 把模块主动上报的东西打出来。
        // +EVENT:MQTT_SUB,<topic>,<len>,<data> 就是订阅到的云端消息。
        n = wifi_read_line(line, sizeof(line), 500);
        if (n > 0)
        {
            // ⚠ 下面这行回显必须在云端消息上脱敏 —— 那是**唯一**会把开锁密码
            //   原样送出 USART1 的地方(AT< 那条也有同样的处理)。
            if (strncmp(line, WIFI_EVT_SUB, sizeof(WIFI_EVT_SUB) - 1) == 0)
            {
                printf("<< ");
                wifi_print_masked(line);
                printf("\r\n");
            }
            else
            {
                printf("<< %s\r\n", line);
            }

            // 云端下发的锁控消息 —— 解析成事务投给 lock_task。
            // **这里只投递,不自己动手** —— 见 lock.h 里 lock_cmd_t 的说明。
            if (strncmp(line, WIFI_EVT_SUB, sizeof(WIFI_EVT_SUB) - 1) == 0)
            {
                wifi_handle_sub_event(line);
                continue;
            }

            // 掉线了就直接返回,让上层把 WiFi + MQTT 整套重来。
            // 模块自己也会尝试重连,但**订阅关系不一定会自动恢复** ——
            // 只重连不重订阅,现象就是"连上了但收不到云端消息",很难查。
            if (strstr(line, "MQTT_DISCONNECT") != NULL)
            {
                printf("!! MQTT 掉线\r\n");
                return;
            }
            continue;   // 有事件就先处理事件,别急着发心跳
        }

        // 到点发一条心跳,证明链路是活的
        if ((GetTim10Tick() - last_pub) >= WIFI_PUB_PERIOD_MS)
        {
            last_pub = GetTim10Tick();
            hb++;

            // 载荷是 JSON。平台一般按 JSON 解析 attributes 话题。
            // 注意:payload 是 AT+MQTTPUB 的**最后一个字段**,里面可以带逗号,
            // 但引号在某些固件版本上会被吃掉 —— 真出问题就先把引号去掉试。
            sprintf(cmd, "AT+MQTTPUB=" WIFI_TOPIC_PUB ",0,0,{\"hb\":%d}", hb);
            wifi_at_cmd(cmd, WIFI_T_PUB_MS);
            printf("<< 已发心跳 #%d\r\n", hb);
        }
    }
}


/**
 * 🟡 L1 架构代码 —— 用轮询而不是等 +EVENT,是因为配网期间必须持续发 AT+WCONFIG? 才知道窗口在不在,而串口的行谁先读谁拿走;另外它的返回值只能说"模块的配网任务开没开",判配网成没成必须另查 AT+WJAP?
 * @brief  查模块当前的配网状态(手册 4.1.16 的 AT+WCONFIG?)
 * @return >=0 = 手册里的 status;-1 = 查询失败
 * @note   status 取值:0=配网任务已关闭 1=WiFi配网(esp touch) 2=蓝牙配网(esp BluFi)
 *                       3=AirKiss  9=Blufi  10=WPS
 *         配网期间只关心两件事:**还是 2(在配网中)** 和 **变成 0(窗口结束了)**。
 * @note   为什么用"轮询状态"而不是"等 +EVENT:WIFI_GOT_IP"?
 *         因为配网期间我们必须**持续发 AT+WCONFIG?** 才知道窗口还在不在,
 *         而串口的行是谁先读谁拿走 —— 两个函数交替读同一个环形缓冲,
 *         必然会把对方的应答吃掉,两边的超时全乱。
 * @note   ⚠ 这个状态**只能说明模块的配网任务开没开,说明不了配网成没成**。
 *         实测:配网成功后模块还会一直报 2,直到有人在 3 分钟窗口外把它关掉。
 *         所以判成功必须另查 AT+WJAP?(见 wifi_ble_provision 里的主判据),
 *         别拿这里的返回值当成功标志。
 */
static int wifi_wifi_config_status(void)
{
    char        cap[64];
    const char *p;
    int         v = 0;

    if (!wifi_at_ex("AT+WCONFIG?", "+WCONFIG:", cap, sizeof(cap), WIFI_T_AT_MS))
    {
        return -1;
    }

    p = strchr(cap, ':');
    if (p == NULL)
    {
        return -1;
    }

    p++;
    while (*p == ' ')
    {
        p++;    // 手册排版里冒号后面有空格,实机输出不一定有,两种都得认
    }

    if ((*p < '0') || (*p > '9'))
    {
        return -1;
    }

    // 自己转数字。不用 atoi:配网状态里 10(WPS)是两位数,而 atoi 遇到
    // 非数字的行为是实现定义的 —— 这里宁可多写四行,换来行为完全可控。
    while ((*p >= '0') && (*p <= '9'))
    {
        v = (v * 10) + (*p - '0');
        p++;
    }

    return v;
}


/**
 * 🟢 L2 工具代码 —— 只倒缓冲不解协议;唯一要守的是"必须等残应答发完再倒",早倒等于把模块的下一句话当成自己的应答
 * @brief  中断过一次 AT 等待之后,把模块那条命令剩下的应答整批倒掉
 * @note   为什么要先等 100ms:wifi_get_byte 是**硬掐断**等待的,模块那边
 *         一个字都还不知道,它接着会把整条应答发完。不发完就倒,这些字节会
 *         落在后面 AT+WCONFIG? 的头上 —— 最坏是一个残留的 "OK" 让这次查询
 *         "命令成功但内容为空",被 wifi_ble_provision 的 st < 0 分支误判成
 *         "固件太老不支持配网",整个配网入口就这么没了。
 */
static void wifi_prov_flush_rx(void)
{
    vTaskDelay(pdMS_TO_TICKS(WIFI_PROV_FLUSH_MS));

    s_rx_tail = s_rx_head;
}

/**
 * 🟢 L2 工具代码 —— 分片睡,只比 vTaskDelay 多一件事:每片问一次"有人按 555# 吗";没人按的话它和 vTaskDelay 完全等价
 * @brief  可被手动配网请求打断的延时
 * @param  ms 最多睡多久
 * @return 1 = 被配网请求打断(没睡够);0 = 睡满了
 * @note   为什么不能让 vTaskDelay 一觉睡 10 秒:那 10 秒里按的 555# 要等睡醒
 *         才被看见,而配网窗口只有 20 秒 —— 等于窗口白白短了一半。
 */
static uint8_t wifi_delay(uint32_t ms)
{
    uint32_t start = GetTim10Tick();

    while ((GetTim10Tick() - start) < ms)
    {
        if (s_prov_req)
        {
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(WIFI_DELAY_SLICE_MS));
    }

    return 0;
}

/**
 * 🟢 L2 工具代码 —— 只画六行字,没有状态;抢不到屏就返回 0 让调用方"活照干、别画界面"
 * @brief  配网开始时占屏并画提示
 * @param  admin_ui 1 = 管理员模式 555# 触发的;0 = 开机自动那次
 * @return 1 = 占到了大提示区(只有 admin_ui 时才可能);0 = 没占屏
 * @note   两条路的画面位置不同,是因为**谁能碰标题行不一样**:
 *           开机那次锁还空闲(s_unlocking == 0),标题行没人抢,改它最省事,
 *           而且不占大提示区就不会和密码输入行打架;
 *           管理员那次锁刚开完、lock_tick() 还在跑,再去写标题行就是两个任务
 *           同时写软件 SPI 的 LCD,画出来是花屏 —— 所以走 lock_screen_take()
 *           占大提示区(全工程统一的那套协议,见 lock.h)。
 * @note   抢不到屏(比如上一次 333# 加卡还在等卡放上来)不报错,串口照样有
 *         全过程,只是屏上没有。
 */
static uint8_t wifi_prov_ui_begin(uint8_t admin_ui)
{
    if (!admin_ui)
    {
        // 屏幕这时停在锁的 "PASSWORD" 界面。不给提示的话,用户只会觉得"锁坏了",
        // 不会想到要掏手机。所以必须占一下屏。
        lock_lcd_set_title((uint8_t *)"BLE PROV", MAGENTA);
        return 0;
    }

    if (!lock_screen_take(LOCK_SCREEN_WIFI))
    {
        printf("配网:屏幕正被别的流程占着,本次只有串口提示\r\n");
        return 0;
    }

    lock_lcd_big_line(0, "BLUETOOTH PROV");
    lock_lcd_big_line(1, "OPEN ESP BLUFI APP");
    lock_lcd_big_line(2, "FIND: " WIFI_PROV_NAME);
    lock_lcd_big_line(3, "SEND WIFI TO IT");
    lock_lcd_big_line(4, "");
    lock_lcd_big_line(5, "");

    return 1;
}

/**
 * 🟢 L2 工具代码 —— 收尾画结果、还屏;成败两条路都走它,保证"离开这个函数时屏幕一定还回去了"
 * @brief  配网结束时的收尾画面
 * @param  admin_ui 同上
 * @param  got_ui   wifi_prov_ui_begin() 的返回值
 * @param  l2/l3    结果两行文字(只有管理员那条路用得上,开机那条路忽略)
 * @note   标题行那条路(开机)不管成没成都得还原:联网失败绝不能连累开门,
 *         屏幕回到能输密码的状态,锁至少还能用。
 */
static void wifi_prov_ui_end(uint8_t admin_ui, uint8_t got_ui,
                             const char *l2, const char *l3)
{
    if (!admin_ui)
    {
        lock_lcd_set_title((uint8_t *)"PASSWORD", BLUE);
        return;
    }

    if (!got_ui)
    {
        return;
    }

    lock_lcd_big_line(2, l2);
    lock_lcd_big_line(3, l3);
    lock_lcd_big_line(4, "");

    vTaskDelay(pdMS_TO_TICKS(WIFI_PROV_MSG_MS)); // 让结果在屏上停一下

    lock_screen_give(LOCK_SCREEN_WIFI);
}

/**
 * 🟡 L1 架构代码 —— 配网流程的骨架:先问固件认不认这条命令、模块已经在配网就别再踢它一脚、成功判据落在 AT+WJAP? 的联网结果上而不是模块的任务状态(实测它会一直赖着报 2),收工无论成败都必须显式关掉窗口,否则设备长期开着"谁都能把它配到别的网"的口子
 * @brief  开一次蓝牙配网,阻塞到配网窗口结束
 * @param  window_ms 配网窗口最多开多久(开机那次 3 分钟,管理员 555# 那次 20 秒)
 * @param  admin_ui  1 = 管理员模式触发的:占大提示区、不碰标题行
 *                   0 = 开机自动那次:改标题行、不占大提示区
 * @return 1 = 配网后模块确实连上 WiFi 了(凭据已经写进 EEPROM);0 = 没成
 * @note   两个调用点:开机 WIFI_PROV_DEADLINE_MS 内没连上 WiFi 时**只调一次**
 *         (由 s_prov_tried 拦着),以及管理员模式 555# 手动触发。
 * @note   手机端要装 **ESP BluFi** App(Espressif 官方的),连上模块开的广播,
 *         把 WiFi 名和密码发过去。模块收到后自己连,连上会发
 *         +EVENT:WIFI_CONNECT / +EVENT:WIFI_GOT_IP。
 * @note   ⚠ 屏幕这条线:原来这里**只有开机那条路**,所以直接写标题行是安全的
 *         —— 那时锁一定空闲(s_unlocking == 0),和 lock_task / finger_task
 *         撞不上。加了 555# 之后这个前提就没了:管理员模式是在 lock_task 还在
 *         跑 tick 的时候进来的,两个任务同时写软件 SPI 的 LCD 就是花屏。
 *         所以现在按 admin_ui 分成两套 —— 这个分叉是必须的,不是洁癖,
 *         见 lock.h 里 LOCK_SCREEN_xxx 的说明。
 */
static uint8_t wifi_ble_provision(uint32_t window_ms, uint8_t admin_ui)
{
    uint32_t start;
    uint8_t  fails = 0;
    uint8_t  got_link = 0;      // 轮询期间就确认"已经连上 WiFi"了
    uint8_t  got_ui;
    int      st;

    if (admin_ui)
    {
        printf("\r\n==== 管理员模式 555#:手动开始蓝牙配网(%u 秒窗口)====\r\n",
               (unsigned)(window_ms / 1000));
    }
    else
    {
        printf("\r\n==== 开机 %u 秒仍未连上 WiFi,转入蓝牙配网 ====\r\n",
               (unsigned)(WIFI_PROV_DEADLINE_MS / 1000));
    }

    got_ui = wifi_prov_ui_begin(admin_ui);

    // ── 先确认固件认不认这条命令 ──
    //    AT+WCONFIG 是 Combo 模块的通用命令,但固件版本老了可能没有。
    //    早一步查出来,比让用户对着屏幕干等 3 分钟强得多。
    //
    //    ⚠ 这一查还有第二个用处,是实测踩出来的:模块的电源**不和 MCU 一起断**。
    //      用户配网配到一半板子复位/掉电,MCU 从零重跑,模块那边的配网任务还开着,
    //      于是这里问出来是 2 而不是 0(手册 4.1.16:0=关 / 1=WiFi 配网 / 2=蓝牙配网)。
    //      要是不看这个值、照样发 AT+WCONFIG=2,模块会回 "+WCONFIG:63 +ERROR"
    //      (63 = 通用错误码,不带任何细节),配网就被误判成"启动失败"直接放弃 ——
    //      而 s_prov_tried 这时已经置位,手机再也没机会配网了。
    st = wifi_wifi_config_status();
    if (st < 0)
    {
        printf("!! 模块不支持 AT+WCONFIG(固件太老?),跳过蓝牙配网\r\n");
        wifi_prov_ui_end(admin_ui, got_ui, "NO AT+WCONFIG", "CHECK FIRMWARE");
        return 0;
    }
    printf("当前配网状态: %d\r\n", st);

    // ── 开一次蓝牙配网 ──
    if (st == 2)
    {
        // 模块已经在配网了(上一次没配完就复位)。这条命令是"开一次",
        // 重复开就是错,直接接着用现成的,别去踢它。
        printf("模块已在蓝牙配网状态,直接等待(不重复启动)\r\n");
    }
    else
    {
        //    第二个参数是广播名。手册说**只有 esp BluFi 协议支持自定义这个名字** ——
        //    起个一眼能认出来的,免得用户在手机上一堆设备里翻。
        if (!wifi_at_cmd("AT+WCONFIG=2," WIFI_PROV_NAME, WIFI_T_CONFIG_MS))
        {
            // 命令报错别急着放弃:63 是"通用错误",信息量约等于没有,
            // 而**模块的真实状态才是判据**。再问一次,进了 2 就照常往下等。
            if (wifi_wifi_config_status() != 2)
            {
                printf("!! 蓝牙配网启动失败\r\n");
                wifi_prov_ui_end(admin_ui, got_ui, "START FAILED", "KEEP OLD WIFI");
                return 0;
            }
            printf("启动命令报错,但模块已进入配网状态,继续等待\r\n");
        }
    }

    printf("蓝牙配网已开启:手机装 ESP BluFi App,搜 \"%s\",填 WiFi 名和密码\r\n",
           WIFI_PROV_NAME);
    printf("最多等 %u 秒\r\n", (unsigned)(window_ms / 1000));

    // ── 等窗口结束 ──
    //    模块**不会**为"配网成功"单独发一个事件 —— 它直接自己连上去。
    //    所以判据只有两条:模块把状态关回 0 了,或者我们自己的上限到了。
    start = GetTim10Tick();
    for (;;)
    {
        if ((GetTim10Tick() - start) >= window_ms)
        {
            printf("!! 蓝牙配网超时:%u 秒没人来配,收工\r\n",
                   (unsigned)(window_ms / 1000));
            break;
        }

        // ── 主判据:WiFi 真的连上了 ──
        //    原来这里是等"模块自己把配网任务关掉(st 变 0)",这个假设**是错的**,
        //    实机日志推翻了它:+EVENT:WIFI_CONNECT / +EVENT:WIFI_GOT_IP 都已经到了,
        //    AT+WCONFIG? 却一直报 2 —— 模块根本没打算自己关。
        //    后果很难查:设备明明配好了,却在这儿干等满 3 分钟,这段时间 wifi_task
        //    整个堵着,MQTT 一直连不上。用户看到的是"手机说配网成功了,设备没反应"。
        //    所以判据必须落在"联网结果"上,而不是"模块的任务状态"上。
        //    这里和下面的 AT+WCONFIG? 是同一个任务里**顺序**发、顺序读,
        //    不会出现 wifi_wifi_config_status() 上面注释担心的"两种读法抢环形缓冲"。
        if (wifi_is_wifi_up())
        {
            printf("---- WiFi 已连上,蓝牙配网成功 ----\r\n");
            got_link = 1;
            break;
        }

        // ── 辅助判据:模块主动把配网窗口关了(它超时了,或它自己认为配完了) ──
        st = wifi_wifi_config_status();

        if (st < 0)
        {
            // 单次查询失败不能就判死刑 —— 模块可能正忙着一轮蓝牙交互,
            // 应答晚了几百毫秒。连续几次都问不到才当作掉线。
            fails++;
            printf("!! 配网状态查询失败(第 %d 次)\r\n", fails);
            if (fails >= 5)
            {
                printf("!! 连续 5 次查不到状态,当作配网结束\r\n");
                break;
            }
        }
        else
        {
            fails = 0;

            if (st == 0)
            {
                // 窗口关了不代表失败 —— 手册说"配网成功/配网超时都会回到关闭"。
                // 到底成没成,交给下面统一的"等几秒再查 AT+WJAP?"去判。
                printf("---- 配网窗口已关闭 ----\r\n");
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(WIFI_T_PROV_POLL_MS));
    }

    // ── 收工:显式把配网任务关掉 ──
    //    不能指望模块自己关(理由见上面主判据)。留着它开着的后果不是"多费点电":
    //    设备会**长期处于可被配网状态**,旁边随便谁装个 esp BluFi App,
    //    就能把设备改到别的 WiFi 上去,连密码都不用知道 —— 这是个一直开着的口子。
    //    放在这个位置(而不是只写在成功分支里)是为了让状态确定:不管这次是配成了、
    //    超时了、还是模块干脆不理我们了,离开这个函数时配网任务都必须是"关"。
    wifi_at_cmd("AT+WCONFIG=0", WIFI_T_CONFIG_MS);

    // ── 窗口关了,给模块几秒把连接建起来再判成败 ──
    //    为什么要等:配网成功的瞬间模块才刚开始连,立刻查 AT+WJAP? 多半是
    //    "正在连"(status 1)。要是这时判成失败,上层就会拿着 EEPROM 里那对
    //    旧凭据去连 —— 用户刚配的新网等于白配。
    //    已经确认连上了(got_link)就跳过 —— 那是刚从 AT+WJAP? 读到的 3,
    //    再等 8 秒纯属让用户多等。
    if (!got_link)
    {
        printf("---- 等 %u 秒,看模块能不能自己连上 ----\r\n",
               (unsigned)(WIFI_PROV_SETTLE_MS / 1000));
        vTaskDelay(pdMS_TO_TICKS(WIFI_PROV_SETTLE_MS));
    }

    if (got_link || wifi_is_wifi_up())
    {
        uint8_t saved;

        printf("---- 蓝牙配网成功,模块已连上 WiFi ----\r\n");

        // **这一处最要紧**:用户刚用手机把 WiFi 名和密码发给了**模块**,
        // 固件手里一个字都没有。不从模块那儿抄一份存进 EEPROM,下次上电
        // 固件就没有凭据可用,只能又掉回配网 —— 用户的辛苦等于白费。
        saved = wifi_ap_creds_save();

        // 存没存进去要分开说。连上了但没存成的现象是"现在能用、一重启就变回
        // 旧网",这种事不说明白,用户只会以为锁坏了 —— 而且下一次上电它真的
        // 会回旧网,那时候再查就晚了。
        wifi_prov_ui_end(admin_ui, got_ui,
                         "PROV OK",
                         saved ? "NEW WIFI SAVED" : "SAVE FAILED");
        return 1;
    }

    printf("---- 蓝牙配网没成 ----\r\n");
    wifi_prov_ui_end(admin_ui, got_ui, "PROV FAILED", "KEEP OLD WIFI");
    return 0;
}

/**
 * 🟡 L1 架构代码 —— s_prov_run 的置位/清位就靠它包住:wifi_ble_provision() 里有三个 return,散着写迟早漏一个,而漏了的现象是"配网结束后再也受理不了新的 555#",从表面完全看不出和这里有关
 * @brief  配网期间把"别受理新请求"的闸门关上
 * @param  window_ms 配网窗口最多开多久
 * @param  admin_ui  1 = 管理员模式触发的
 * @return wifi_ble_provision() 的返回值
 * @note   放在 wifi_ble_provision() **后面**是为了不用前置声明;调用方
 *         (wifi_task)更靠后,所以顺序上没问题。
 */
static uint8_t wifi_ble_provision_run(uint32_t window_ms, uint8_t admin_ui)
{
    uint8_t ok;

    s_prov_run = 1;
    ok = wifi_ble_provision(window_ms, admin_ui);
    s_prov_run = 0;

    return ok;
}


/**
 * 🟡 L1 架构代码 —— 联网业务的总编排:{探活 → 连 WiFi → 连 MQTT → 常驻收发} 失败就整体重来、永不退出;凭据从 EEPROM 读、"配网只自动做一次"、开机计时用 TIM10 而不是调度器 tick,都是在这一层定的
 * @brief  WiFi + MQTT 任务
 * @note   优先级 1,比所有业务任务都低。联网再慢也不影响开锁。
 * @note   结构:{ [探活] → [连 WiFi] → [连 MQTT] → [常驻收发] } 循环重试,**永不退出**。
 *         任何一步失败都等一会儿整体重来:模块起得慢、线没插好、热点没开、
 *         路由器重启、云端踢人,都是常态。任务一死就再没人重连,
 *         设备在用户眼里就是"坏了"。
 * @note   **凭据从哪来**:开机第一件事就是把 WiFi 名/密码从 EEPROM 里读出来
 *         (见 wifi_cfg.c),以后每次重连都用这一份。
 *         连不上 → 开一次蓝牙配网(见 wifi_ble_provision),配成功后
 *         固件把新凭据抄回来存进 EEPROM,下次上电就用新的。
 *         所以"换 WiFi"这个动作只需要做一次,不用重新烧录固件。
 * @note   配网只自动做一次 —— 每次都进的话,设备会没完没了地变成可被配网状态,
 *         既费电又开着个安全口子。
 * @note   **另一个入口是管理员模式 555#**(见 wifi_request_provision):
 *         换 WiFi 不用重新烧录,也不用等开机那次。这条请求是跨任务来的,
 *         所以循环顶上先查它,而且"等 10 秒重试"那种长延时被换成了
 *         wifi_delay() 分片睡 —— 不然 20 秒的配网窗口会被睡掉一半。
 */
static void wifi_task(void *arg)
{
    // ── 配网窗口的计时起点 ──
    //    用 GetTim10Tick() 而不是 xTaskGetTickCount(),两个原因:
    //      1) TIM10 那个 1ms 时基是**裸机阶段就起来的**(main 一开头就配),
    //         所以这个起点更接近"开机"本身,而不是"调度器起来"。
    //         main 里那几秒外设初始化(LCD、EEPROM、指纹模块)也算得进去,
    //         这正是用户说"开机 15 秒"时脑子里想的东西。
    //      2) 连 WiFi 那一步的预算也是用 GetTim10Tick() 算的,同一个时基,
    //         少一层换算就少一个出错的地方。
    uint32_t boot = GetTim10Tick();
    char     ssid[WIFI_CFG_SSID_MAX + 1];
    char     pwd[WIFI_CFG_PWD_MAX + 1];
    uint8_t  have_cred;
    uint8_t  up;

    (void)arg;

    // ── 开机第一件事:把 WiFi 名和密码从 EEPROM 读出来 ──
    //    I2C 总线在 main 里已经配好了(wb2_Config 之后那行),这里可以直接读。
    //    读一次只要几毫秒(读没有写周期,tWR 只在写的时候要等)。
    //    这个任务是**调度器起来之后唯一碰 EEPROM 的** —— 锁只在裸机阶段
    //    lock_init() 里读写一次,所以不用考虑两条 I2C 事务打架。
    have_cred = wifi_cfg_load(ssid, pwd);
    if (have_cred)
    {
        printf("已从 EEPROM 读到 WiFi:[%s]\r\n", ssid);
    }
    else
    {
        printf("EEPROM 里还没有 WiFi 凭据,待会儿要先配网\r\n");
    }

    // ── 等模块上电就绪 ──
    vTaskDelay(pdMS_TO_TICKS(WIFI_BOOT_MS));

    printf("\r\n==== WB2 联网开始 ====\r\n");
    printf("(开机 %u 秒内连不上 WiFi,就转蓝牙配网)\r\n",
           (unsigned)(WIFI_PROV_DEADLINE_MS / 1000));

    // ── 重试环:连"模块没应答"也在里面重试,永不退出 ──
    //    为什么连探活失败都要重试?
    //      1) 模块可能比 MCU 起得慢。板子和模块一起上电时,如果模块的 AT
    //         框架还没就绪,一次探活失败就判死刑,之后模块起来了也没人理它。
    //      2) 线没插好/没共地是最常见的情况,插上之后不该还要求重启板子。
    //    代价只是这 2KB 栈一直被占着 —— 比起"插好了却要重启",这点代价很值。
    //    (优先级 1,就算永远在这儿打转也抢不了任何业务任务的 CPU。)
    for (;;)
    {
        // ── 管理员模式 555# 派下来的手动配网:最优先 ──
        //    放在循环最顶上,是要让它在**任何状态下**都能被受理 ——
        //    wifi_task 平时可能在等 10 秒重试、可能在 25 秒的 WJAP 里、
        //    也可能卡在下面那个永不退出的 wifi_session() 里。
        if (wifi_take_prov_request())
        {
            // 上一条命令的应答可能还在路上(我们刚才是硬掐断它的等待的)。
            // 不等它发完就直接开配网,那些残字节会被 AT+WCONFIG? 当成自己的
            // 应答 —— 最坏是一个残留的 "OK" 让查询"成功但没内容",被误判成
            // "固件不支持配网"直接放弃。理由详见 wifi_prov_flush_rx()。
            wifi_prov_flush_rx();

            if (wifi_ble_provision_run(WIFI_PROV_ADMIN_MS, 1))
            {
                // 配网成功:新凭据已经在 EEPROM 里了,重新读一份到局部变量,
                // 下面那趟就用新的连 —— 这就是"成功即修改 rom,使用新网络"。
                have_cred = wifi_cfg_load(ssid, pwd);
            }

            // 不管成没成都回循环顶重走一遍:成了就用新网连,没成就拿手上
            // 这对旧凭据继续重试 —— 也就是"配网失败就继续使用当前网络"。
            // (失败时不重新 load,ssid/pwd 还是原来的内容。)
            continue;
        }

        if (!wifi_at_cmd("AT", WIFI_T_AT_MS))
        {
            printf("!! 模块没应答,检查 PA2/PA3 是否和 WB2 的 RX/TX 交叉接好、是否共地\r\n");
            printf("   %dms 后重试\r\n", WIFI_RETRY_MS);

            // 分片睡:整整 10 秒里按的 555# 得等到睡醒才被看见,
            // 而配网窗口一共才 20 秒。被请求打断的话睡不满也没关系 ——
            // 下面那句 continue 本来就要回循环顶,那里会受理它。
            wifi_delay(WIFI_RETRY_MS);
            continue;
        }

        // 切 STA 模式(1=STA),并存进 flash,下次上电不用重设
        wifi_at_cmd("AT+WMODE=1,1", WIFI_T_CONFIG_MS);

        // ── 1. 用 EEPROM 里的凭据连 WiFi ──
        //    wifi_wifi_join() 内部自带"已经连着就不发命令"的判断,
        //    所以这里不用先查一次 —— 一次循环只发一条 AT+WJAP? 探针。
        if (have_cred)
        {
            up = wifi_wifi_join(ssid, pwd, s_prov_tried ? WIFI_T_WJAP_MS
                                                        : wifi_join_budget(boot));
        }
        else
        {
            // 没凭据可发,只能寄希望于"模块自己连上了"(它的 flash 里可能存着)
            up = wifi_is_wifi_up();
        }

        // ── 2. WiFi 通了就连 MQTT,连上就常驻收发 ──
        //    && 会短路:WiFi 没通就不去连 MQTT 了(反正必然失败,还白等 15s)
        if (up && wifi_mqtt_connect())
        {
            wifi_session();     // 常驻收发,只有 MQTT 掉线(或管理员要配网)才返回

            // 从上面出来只说明 MQTT 掉了,WiFi 未必也掉了 —— 重新问一次,
            // 免得下面把"MQTT 掉线"误判成"WiFi 连不上"而去开蓝牙配网。
            up = wifi_is_wifi_up();
        }

        // ── 有人按了 555# → 回循环顶把它办掉 ──
        //    这一步必须夹在这里,不能只写在 wifi_session() 里:
        //    上面**任何一条 AT 命令**都可能被 555# 掐断(wifi_get_byte 是硬
        //    中断等待的),掐断的结果会一路装成"连不上"。掉进下面那条路的话,
        //    轻则白等 10 秒,重则拿这个假失败去开开机那次**3 分钟**的自动配网
        //    —— 而用户按 555# 想要的明明是 20 秒那次。
        if (s_prov_req)
        {
            continue;
        }

        // ── 3. 还是没连上 → 开一次蓝牙配网 ──
        //    拦重复触发用的是 s_prov_tried(试过就算),不是"配成才置位"的标志:
        //    不这样,用户没掏手机就会无限循环进配网,每次卡满 3 分钟。
        //    注意**没有**"必须等满 15 秒"这个条件 —— 没有凭据时
        //    (第一次上电)会立刻走到这里,这是对的:没什么可试的,直接让用户配。
        if (!s_prov_tried && !up)
        {
            s_prov_tried = 1;   // 进门就置位:哪怕配网中途异常返回,也只做这一次

            if (wifi_ble_provision_run(WIFI_T_PROV_MS, 0))
            {
                // 配网成功,凭据已经由 wifi_ap_creds_save() 写进 EEPROM 了,
                // 这里重新读一份到局部变量,下面几轮循环就用新的连。
                have_cred = wifi_cfg_load(ssid, pwd);
            }

            // 别等那 10 秒了,马上按新状态试一次
            continue;
        }

        printf("!! 联网未就绪,%dms 后重试\r\n", WIFI_RETRY_MS);
        wifi_delay(WIFI_RETRY_MS);  // 分片睡,见函数说明
    }
}


/**
 * 🔴 L0 生死线代码 —— PA2/PA3 的 AF7 复用号、USART2 挂 APB1(不是 USART1/6 那个 APB2)、NVIC 抢占优先级 3、只开 RXNE 不开 IDLE,四处都得对着手册和原理图核;错一处就是"偶尔丢字节"这种最难查的症状
 * @brief  初始化 USART2:PA2=TX / PA3=RX,AF7
 */
void wb2_Config(void)
{
    GPIO_InitTypeDef  gpio  = {0};
    USART_InitTypeDef usart = {0};
    NVIC_InitTypeDef  nvic  = {0};

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);

    // ⚠ USART2 挂在 **APB1**,而 USART1/USART6 挂在 APB2 —— 别抄错那一行。
    //   (波特率不用自己算除数:StdPeriph 的 USART_Init() 会去读 RCC 寄存器
    //    反推 PCLK1,所以就算以后改主频,这里也不用动。)
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

    gpio.GPIO_Mode  = GPIO_Mode_AF;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_Speed = GPIO_Low_Speed;
    // RX 上拉:模块不发数据时线是高的,上拉能让空闲电平确定,
    // 避免悬空引脚收进起始位误判的乱码
    gpio.GPIO_PuPd  = GPIO_PuPd_UP;
    gpio.GPIO_Pin   = GPIO_Pin_2 | GPIO_Pin_3;

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource2, GPIO_AF_USART2);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource3, GPIO_AF_USART2);
    GPIO_Init(GPIOA, &gpio);

    usart.USART_BaudRate            = WIFI_UART_BAUD;
    usart.USART_WordLength          = USART_WordLength_8b;
    usart.USART_StopBits            = USART_StopBits_1;
    usart.USART_Parity              = USART_Parity_No;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART2, &usart);

    // 抢占优先级 3:和 USART1 同级,低于 TIM10(1)、高于业务任务。
    // 收一个字节就出中断,活儿极短,不会影响时序。
    nvic.NVIC_IRQChannel                   = USART2_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 3;
    nvic.NVIC_IRQChannelSubPriority        = 0;   // 分组 4 下这个字段被忽略,写上是为了不留未初始化值
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);

    // 只开 RXNE。**不开 IDLE 中断** —— 环形缓冲不需要按"一帧结束"来切分,
    // 按行解析比按帧解析更贴合 AT 命令(模块本来就用 \r\n 分行)。
    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);

    USART_Cmd(USART2, ENABLE);
}


// 🟡 L1 架构代码 —— 512 字栈是按 wifi_session→wifi_at_ex 的最深调用链再加几个任务共用 fputc 的 printf 开销估出来的(栈溢出不报错、只改坏别人的数据),优先级给 1 是为了联网永远抢不走业务任务的 CPU
void wifi_tasks_init(void)
{
    // 栈 512 字(2048 字节)。为什么要这么多? 最深的调用链是
    //     wifi_session(cmd[160] + line[128])
    //       → wifi_at_cmd → wifi_at_ex(line[128])
    // 光局部数组就 416 字节,再叠上 printf 内部的缓冲(几个任务的 printf
    // 都走同一个 fputc,microlib 在里面还要用几十上百字节)就逼近 384 字了。
    // 栈溢出**不报错、只改坏别人的数据**,是 FreeRTOS 最难查的一类 bug,
    // 所以宁可多给 512 字节 —— 堆有 12KB,这点余量付得起。
    xTaskCreate(wifi_task, "wifi", 512, NULL, 1, NULL);
}
