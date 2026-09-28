#include "wifi.h"
#include "tim.h"        // GetTim10Tick():裸机计时用的 1ms 时基
#include "lock.h"       // lock_post_cmd():把云端下发翻译成锁控命令
#include "FreeRTOS.h"
#include "task.h"
#include "stdio.h"
#include "string.h"

// ══════════════════════════════════════════════════════════════════════════
//  分包平台参数(改这里就能换平台/换 WiFi,不用动逻辑)
//
//  下面这些是接入方给的接入信息,MQTT 的 Client ID 用 mqtt.fx 生成的那个。
//  ⚠ 密码是明文躺在源码里的 —— 教学项目无所谓,真做产品要么编译期加密,
//    要么出厂时写进 EEPROM 由用户自己配。
// ══════════════════════════════════════════════════════════════════════════

// ⚠ SSID 中间**有一个空格**:Windows 移动热点默认会把名字写成
//   "<电脑名> <4位数字>"。空格在 AT 命令里是危险字符(参数本来靠逗号分隔),
//   所以下面连 WiFi 的地方会特意把"带引号"的写法也备一份。
#define WIFI_SSID           "DESKTOP-HTLNPUV 4127"
#define WIFI_PASSWORD       "88888888"

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
//        +EVENT:MQTT_SUB,attributes/push,14,{"DoorLock":0}
//        └──── 前缀 ────┘└── topic ────┘└len┘└── payload ──┘
//
//    ⚠ len 是**平台声明的载荷字节数**,不是我们自己数的 —— 拿它和实际收到的
//      长度对账,就能发现"行缓冲太小、载荷被截断"这种静默错误。
//      见 wifi_handle_sub_event()。
#define WIFI_EVT_SUB        "+EVENT:MQTT_SUB,"  // ⚠ 结尾的逗号算在常量里
#define WIFI_FIELD_DOORLOCK "\"DoorLock\""      // ⚠ 引号一起带上,免得误命中别的前缀

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
#define WIFI_T_SCAN_MS      12000   // 扫描周围 AP(诊断用,要扫好几个信道,给足)

#define WIFI_PUB_PERIOD_MS  10000   // 心跳周期:10s 一条
#define WIFI_RETRY_MS       10000   // 联网失败后多久重试一次

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

// ── 黑匣子:USART2 收到的字节原样留一份,写满为止,永不覆盖 ──
//    为什么环形缓冲不够用? 因为正常路径下 wifi_task 会**实时把它抽干**并
//    转发到 USART1 —— 事后 dump 环形缓冲,只能看到"此刻还没被读走"的几个
//    字节,历史全没了。想离线复盘模块到底说过什么(比如扫描结果),就得有
//    一份不被消费的副本。
//    1KB 只够存一次完整的扫描列表;写满后停止记录,靠调试器读地址 dump 出来。
//    不需要时删掉这个数组和 ISR 里那两行即可,不影响功能。
#define WIFI_LOG_SIZE       1024
static volatile uint8_t  s_log[WIFI_LOG_SIZE];
static volatile uint16_t s_log_len = 0;


/**
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

        // 黑匣子:先进先出,写满就不再记(保留最早的一段,方便从开机看起)
        if (s_log_len < WIFI_LOG_SIZE)
        {
            s_log[s_log_len] = b;
            s_log_len++;
        }

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
 * @brief  从环形缓冲取一个字节,取不到就等到超时
 * @param  out        输出:取到的字节
 * @param  start      本轮计时的起点,来自 GetTim10Tick()
 * @param  timeout_ms 从 start 起最多等多久
 * @return 1 = 取到了;0 = 到点还没有
 * @note   **只能在任务上下文调用** —— 缓冲空的时候用 vTaskDelay 让出 CPU。
 *         不用忙等:联网这种"等几百毫秒很正常"的场合,忙等会把
 *         同优先级和低优先级的任务全饿死。
 */
static uint8_t wifi_get_byte(uint8_t *out, uint32_t start, uint32_t timeout_ms)
{
    while (s_rx_head == s_rx_tail)
    {
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
            printf("AT< %s\r\n", line);

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
 * @brief  wifi_at_ex 的简化版:不关心应答内容,只想知道成没成
 */
static uint8_t wifi_at_cmd(const char *cmd, uint32_t timeout_ms)
{
    return wifi_at_ex(cmd, NULL, NULL, 0, timeout_ms);
}


/**
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
 * @brief  连 WiFi:已经连着就不动,没连才发 AT+WJAP
 * @return 1 = WiFi 通了;0 = 没通(失败原因已打到控制台)
 * @note   为什么要先查一次 AT+WJAP? 而不是每次都直接连?
 *         有些固件在"已连接"状态下重发 WJAP 会报错,那样就把好好的连接
 *         误判成失败,陷入"明明能用却一直重连"的死循环。
 */
static uint8_t wifi_wifi_join(void)
{
    if (wifi_is_wifi_up())
    {
        printf("---- WiFi 已连接,跳过 ----\r\n");
        return 1;
    }

    printf("---- 连接 WiFi: [%s] ----\r\n", WIFI_SSID);
    s_at_prev[0] = '\0';

    // 写法一:裸写。手册 1.3 节只要求"参数里含**逗号**才必须加双引号",
    // 空格理应原样保留 —— 按理说这样就够了。
    if (wifi_at_cmd("AT+WJAP=" WIFI_SSID "," WIFI_PASSWORD, WIFI_T_WJAP_MS))
    {
        printf("---- WiFi 连上了 ----\r\n");
        return 1;
    }

    // 失败时别只说"失败了" —— 模块其实把原因告诉我们了,只是藏在一个数字里。
    wifi_show_errno(s_at_prev);

    // 写法二:整个 SSID 加双引号。
    // 为什么要多此一举? 因为有些固件的解析器会把参数**首尾的空格**当分隔符吃掉,
    // 于是 "DESKTOP-HTLNPUV 4127" 被截成 "DESKTOP-HTLNPUV" ——
    // 现象是最坑的一种:"我明明把名字写对了,就是连不上"。
    // 引号能把整串锁死成一个参数,谁也吃不掉。手册给的失败码里没有"SSID 不匹配"
    // 这一项(只区分 66 连不上 / 69 没扫到),从报错看是分辨不出来的,只能试。
    printf("---- 再试一次(SSID 加双引号)----\r\n");
    if (wifi_at_cmd("AT+WJAP=\"" WIFI_SSID "\"," WIFI_PASSWORD, WIFI_T_WJAP_MS))
    {
        printf("---- WiFi 连上了(引号写法)----\r\n");
        return 1;
    }

    wifi_show_errno(s_at_prev);

    // 再扫一把周围有哪些 AP。扫描结果里带**信道号**:
    // 1~14 是 2.4G,36 及以上是 5G —— 一眼就能看出是不是频段不对。
    // 同时也能核对 SSID 的真实拼写(有没有多出空格、后缀之类)。
    printf("---- 扫一下周围的 AP(信道 1~14 才是 2.4G)----\r\n");
    wifi_at_cmd("AT+WSCAN", WIFI_T_SCAN_MS);

    return 0;
}


/**
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
 * @brief  把云端下发的 JSON 载荷翻译成锁控命令,投给 lock_task
 * @param  payload 形如 {"DoorLock":1} 的文本
 * @note   平台约定(定义在 lock.h):
 *             {"DoorLock":1}  → 开锁
 *             {"DoorLock":0}  → 关锁
 * @note   为什么不上 cJSON? 载荷就一个字段,为它拉一个 JSON 库进来
 *         (还有 malloc 和栈的代价)不划算。但**这种手写解析只在载荷形状
 *         完全可控时才成立** —— 一旦云端改字段名、载荷里出现嵌套对象,
 *         或者要处理字符串转义,就得换成真正的 JSON 解析器。
 * @note   这里是"尽力而为"的解析:认不出来就打印原文并放弃,**绝不瞎猜**。
 *         猜错的代价是误开锁。
 */
static void wifi_handle_downlink(const char *payload)
{
    const char *p;
    const char *q;

    // 找 "DoorLock"(引号一起找,免得将来云端多个 "DoorLockState"
    // 之类的字段时被误命中)
    p = strstr(payload, WIFI_FIELD_DOORLOCK);
    if (p == NULL)
    {
        printf("!! 载荷里没有 DoorLock 字段,忽略: %s\r\n", payload);
        return;
    }

    p += sizeof(WIFI_FIELD_DOORLOCK) - 1;   // 跳过 "DoorLock"

    // 跳过冒号和空白:"DoorLock" : 1 这种带空格的写法也要认。
    // 判空是为了防着载荷被截断成 {"DoorLock" 这种半截样子时越界读。
    while ((*p == ' ') || (*p == '\t') || (*p == ':'))
    {
        p++;
    }

    // 值可能是数字(1/0),也可能是**带引号的字符串**("1"/"0")——
    // 有些平台的前端会把开关量当字符串下发。两种都认。
    q = (*p == '"') ? (p + 1) : p;

    if ((*q != '0') && (*q != '1'))
    {
        printf("!! DoorLock 的值不是 0/1,忽略: %s\r\n", payload);
        return;
    }

    if (*q == '1')
    {
        printf("云端下发:开锁\r\n");
        lock_post_cmd(LOCK_CMD_OPEN);
    }
    else
    {
        printf("云端下发:关锁\r\n");
        lock_post_cmd(LOCK_CMD_CLOSE);
    }
}


/**
 * @brief  处理一行 +EVENT:MQTT_SUB 上报(云端有消息下来了)
 * @param  line 完整一行,形如:
 *              +EVENT:MQTT_SUB,attributes/push,14,{"DoorLock":0}
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

    // 跳过 "+EVENT:MQTT_SUB,"
    topic = line + (sizeof(WIFI_EVT_SUB) - 1);

    // ── 第 1 个逗号:topic 到此为止 ──
    comma = strchr(topic, ',');
    if (comma == NULL)
    {
        printf("!! 上报格式不对(少了长度字段): %s\r\n", line);
        return;
    }
    topic_len = (uint32_t)(comma - topic);

    // 只处理我们订阅的那个话题。别的话题说明模块里的订阅配置和预期不一致
    // (多半是模块里还留着上一次跑留下的订阅),打印出来而不是闷头处理。
    if ((topic_len != strlen(WIFI_TOPIC_SUB)) ||
        (strncmp(topic, WIFI_TOPIC_SUB, topic_len) != 0))
    {
        printf("!! 收到非本端订阅话题的消息,忽略: %s\r\n", line);
        return;
    }

    // ── 第 2 个逗号:长度字段到此为止,后面全是 payload ──
    len_str = comma + 1;
    comma   = strchr(len_str, ',');
    if (comma == NULL)
    {
        printf("!! 上报格式不对(少了载荷): %s\r\n", line);
        return;
    }
    payload = comma + 1;

    // ── 长度字段转数字 ──
    // 不引 atoi:它遇到非数字的行为是实现定义的,这里只认纯数字,自己判更稳。
    for (; len_str < comma; len_str++)
    {
        if ((*len_str < '0') || (*len_str > '9'))
        {
            printf("!! 长度字段不是数字,整条丢弃: %s\r\n", line);
            return;
        }
        want_len = (want_len * 10) + (uint32_t)(*len_str - '0');
    }

    got_len = (uint32_t)strlen(payload);

    printf("云端消息:话题=%s,%u 字节,载荷=%s\r\n",
           WIFI_TOPIC_SUB, got_len, payload);

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
 * @brief  MQTT 就绪之后的常驻循环:转发上报 + 定时发心跳
 * @note   只有 MQTT 掉线才返回,交给上层重连。
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
        // 把模块主动上报的东西打出来。
        // +EVENT:MQTT_SUB,<topic>,<len>,<data> 就是订阅到的云端消息。
        n = wifi_read_line(line, sizeof(line), 500);
        if (n > 0)
        {
            printf("<< %s\r\n", line);

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
 * @brief  WiFi + MQTT 任务
 * @note   优先级 1,比所有业务任务都低。联网再慢也不影响开锁。
 * @note   结构:{ [探活] → [连 WiFi] → [连 MQTT] → [常驻收发] } 循环重试,**永不退出**。
 *         任何一步失败都等一会儿整体重来:模块起得慢、线没插好、热点没开、
 *         路由器重启、云端踢人,都是常态。任务一死就再没人重连,
 *         设备在用户眼里就是"坏了"。
 */
static void wifi_task(void *arg)
{
    (void)arg;

    // ── 等模块上电就绪 ──
    vTaskDelay(pdMS_TO_TICKS(WIFI_BOOT_MS));

    printf("\r\n==== WB2 联网开始 ====\r\n");

    // ── 重试环:连"模块没应答"也在里面重试,永不退出 ──
    //    为什么连探活失败都要重试?
    //      1) 模块可能比 MCU 起得慢。板子和模块一起上电时,如果模块的 AT
    //         框架还没就绪,一次探活失败就判死刑,之后模块起来了也没人理它。
    //      2) 线没插好/没共地是最常见的情况,插上之后不该还要求重启板子。
    //    代价只是这 2KB 栈一直被占着 —— 比起"插好了却要重启",这点代价很值。
    //    (优先级 1,就算永远在这儿打转也抢不了任何业务任务的 CPU。)
    for (;;)
    {
        if (!wifi_at_cmd("AT", WIFI_T_AT_MS))
        {
            printf("!! 模块没应答,检查 PA2/PA3 是否和 WB2 的 RX/TX 交叉接好、是否共地\r\n");
            printf("   %dms 后重试\r\n", WIFI_RETRY_MS);
            vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_MS));
            continue;
        }

        // 切 STA 模式(1=STA),并存进 flash,下次上电不用重设
        wifi_at_cmd("AT+WMODE=1,1", WIFI_T_CONFIG_MS);

        // && 会短路:WiFi 没通就不去连 MQTT 了(反正必然失败,还白等 15s)
        if (wifi_wifi_join() && wifi_mqtt_connect())
        {
            wifi_session();     // 常驻收发,只有 MQTT 掉线才返回
        }

        printf("!! 联网未就绪,%dms 后重试\r\n", WIFI_RETRY_MS);
        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_MS));
    }
}


/**
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
