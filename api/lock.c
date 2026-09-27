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
 * @brief  开锁:电机推出锁舌 → 停 3 秒 → 收回锁舌
 * @note   全程用 vTaskDelay 让出 CPU。
 *         **不能用 L9110S_move()** ——它内部是
 *         `while (GetTim10Tick() - start < ms);` 的忙等,会把 CPU 整整占死
 *         3 秒,同优先级和低优先级的任务全部饿死(包括触摸板扫描任务,
 *         那就是"开锁期间按键没反应")。
 *         这和 AT24C04 里 Delay_Ms 要换成 vTaskDelay 是同一个道理。
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

    lock_lcd_set_title(title, color);
    lock_clear_input();

    // 开锁行程:正转推到位,然后刹车停住(刹车比自由滑行可靠,锁舌不会溜回去)
    L9110S_control(MOTOR_FORWARD);
    vTaskDelay(pdMS_TO_TICKS(LOCK_MOTOR_MS));
    L9110S_control(MOTOR_BRAKE);

    printf("开锁完成,保持 %d 秒后自动回锁\r\n", LOCK_OPEN_MS / 1000);
    vTaskDelay(pdMS_TO_TICKS(LOCK_OPEN_MS));

    // 回锁行程
    L9110S_control(MOTOR_REVERSE);
    vTaskDelay(pdMS_TO_TICKS(LOCK_MOTOR_MS));
    L9110S_control(MOTOR_BRAKE);

    printf("已回锁\r\n");

    lock_lcd_set_title((uint8_t *)"PASSWORD", BLUE);
    lock_clear_input();

    s_unlocking = 0;
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
 * @brief  密码锁任务:消费触摸板按键事件
 * @note   平时阻塞在 xQueueReceive 上,不占 CPU。优先级 2,和 cy9cm_scan
 *         同级 —— 队列一有数据就绪,立即被唤醒。
 */
static void lock_task(void *arg)
{
    uint8_t key;

    for (;;)
    {
        if (xQueueReceive(cy9cm_key_queue, &key, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        // 正在开锁/回锁的过程中,按键一律忽略
        if (s_unlocking)
        {
            continue;
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
}


void lock_tasks_init(void)
{
    // 栈 320 字(1280 字节):lock_unlock() 里会调到 LCD_Font_Dis_Ascii(),
    // 它自带 100 字节的 buff,再加上 printf,留足余量。
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
