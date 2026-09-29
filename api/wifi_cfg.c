#include "wifi_cfg.h"
#include "AT24C04.h"    // AT24C04_read / AT24C04_crosspage_write
#include "stdio.h"
#include "string.h"

// ── EEPROM 里的字段地址 ──
//    地址 0~7 是锁密码(见 lock.c 开头的布局说明),WiFi 从 8 往后排,互不重叠。
#define WIFI_CFG_SSID_FIELD (WIFI_CFG_SSID_MAX + 1)     // 33:多留一个 '\0'
#define WIFI_CFG_PWD_FIELD  (WIFI_CFG_PWD_MAX + 1)      // 64:同上

#define WIFI_CFG_ADDR       8                                                   // 整块起点
#define WIFI_CFG_DATA_BYTES (WIFI_CFG_SSID_FIELD + WIFI_CFG_PWD_FIELD)          // 97:SSID + 密码
#define WIFI_CFG_FLAG_ADDR  (WIFI_CFG_ADDR + WIFI_CFG_DATA_BYTES)               // 105:标志跟在数据后面

// 标志值不能和锁的 0xA5 撞 —— 两个模块共用一片 EEPROM,一旦读串了地址,
// 标志不一样至少能当场发现,不会把锁密码当成 WiFi 名。
#define WIFI_CFG_FLAG_VALUE 0xA6u

// 读写用的中间缓冲。**故意放静态区不放栈上**:wifi_task 的栈一共就 2KB
// (见 wifi_tasks_init 里的说明),这 97 字节压上去会把余量吃掉一大块。
// 全局只有 wifi_task 碰 EEPROM(锁只在裸机阶段读写一次),不存在重入。
static uint8_t s_buf[WIFI_CFG_DATA_BYTES];


/**
 * 🟡 L1 架构代码 —— 它的 0/1 决定 wifi_task 下一步是照常联网还是去开蓝牙配网;先验标志再读那 97 字节是为开机路径省一次 I2C,两个字段尾字节钉 '\0' 是防 EEPROM 里上次写坏的残渣把 strcpy 冲出去
 * @brief  从 EEPROM 读 WiFi 凭据
 * @param  ssid 输出:WiFi 名(至少 WIFI_CFG_SSID_MAX + 1 字节)
 * @param  pwd  输出:密码  (至少 WIFI_CFG_PWD_MAX  + 1 字节)
 * @return 1 = 有效;0 = 没存过 / 读失败 / 内容不合法
 * @note   先读标志再读数据:标志不对就不用去碰那 97 字节了,
 *         一次 I2C 读换一次直接返回,开机路径上快一点。
 */
uint8_t wifi_cfg_load(char *ssid, char *pwd)
{
    uint8_t flag = 0;

    if (AT24C04_read(WIFI_CFG_FLAG_ADDR, &flag, 1) != 0)
    {
        return 0;   // 器件没应答,当没存过处理,上层自然会去配网
    }

    if (flag != WIFI_CFG_FLAG_VALUE)
    {
        return 0;
    }

    if (AT24C04_read(WIFI_CFG_ADDR, s_buf, WIFI_CFG_DATA_BYTES) != 0)
    {
        return 0;
    }

    // 定长字段靠 '\0' 收尾,但里面可能是上一次写坏的内容,
    // 两个字段的最后一个字节先钉成 '\0',保证后面 strcpy 不会冲出去。
    s_buf[WIFI_CFG_SSID_FIELD - 1] = '\0';
    s_buf[WIFI_CFG_DATA_BYTES - 1] = '\0';

    if (s_buf[0] == '\0')
    {
        return 0;   // 空 SSID 等于没存过
    }

    strcpy(ssid, (char *)s_buf);
    strcpy(pwd, (char *)(s_buf + WIFI_CFG_SSID_FIELD));

    return 1;
}


/**
 * 🟡 L1 架构代码 —— 三步写序(清标志 → 写数据 → 落标志)是掉电安全的本体:写 97 字节要跨 7 个 EEPROM 页、每页都要等器件忙 10ms,配网刚成功时用户可能正碰板子;顺序反了就会读出"新 SSID + 旧密码"且无法自愈
 * @brief  把 WiFi 凭据写进 EEPROM
 * @param  ssid WiFi 名;  pwd 密码
 * @return 1 = 写成功;0 = 长度不合法(一个字都没写)
 * @note   **为什么分三步写、标志还放在最后** —— 这是为了防"写到一半掉电":
 *         写 97 字节要横跨 7 个 EEPROM 页,每页之间器件都要忙 10ms。这期间
 *         断电是完全可能的(配网刚成功,用户可能正拿手碰板子)。
 *         如果标志和数据在同一次写里落盘,掉电后可能读出
 *         "新 SSID + 旧密码" 这种缝合怪 —— 标志有效,内容却是错的,
 *         设备会拿着一个不存在的密码反复连网,怎么配都配不好,极难查。
 *         所以顺序是:**先清标志 → 写数据 → 最后落标志**。
 *         任何时候掉电,要么标志还是 0(上层当没存过,重新配网),
 *         要么数据已经完整写完。中间态读不出来。
 */
uint8_t wifi_cfg_save(const char *ssid, const char *pwd)
{
    const uint8_t *p;
    uint16_t       slen = (uint16_t)strlen(ssid);
    uint16_t       plen = (uint16_t)strlen(pwd);
    uint16_t       i;
    uint8_t        flag = WIFI_CFG_FLAG_VALUE;
    uint8_t        zero = 0x00u;

    if ((slen == 0) || (slen > WIFI_CFG_SSID_MAX) || (plen > WIFI_CFG_PWD_MAX))
    {
        printf("!! WiFi 凭据长度不合法(名字 %u 字节 / 密码 %u 字节),不写 EEPROM\r\n",
               (unsigned)slen, (unsigned)plen);
        return 0;
    }

    // 定长字段:先整体清 0,再把内容填到前面,剩下的自然就是补的 0
    memset(s_buf, 0, sizeof(s_buf));
    p = (const uint8_t *)ssid;
    for (i = 0; i < slen; i++)
    {
        s_buf[i] = p[i];
    }
    p = (const uint8_t *)pwd;
    for (i = 0; i < plen; i++)
    {
        s_buf[WIFI_CFG_SSID_FIELD + i] = p[i];
    }

    // ── 第 1 步:清标志(单字节页写也能用,反正标志自己占一个地址) ──
    AT24C04_page_write(WIFI_CFG_FLAG_ADDR, &zero, 1);

    // ── 第 2 步:写数据。97 字节跨 7 页,crosspage_write 会自动按页边界拆 ──
    AT24C04_crosspage_write(WIFI_CFG_ADDR, s_buf, WIFI_CFG_DATA_BYTES);

    // ── 第 3 步:落标志。到这一步数据已经完整了 ──
    AT24C04_page_write(WIFI_CFG_FLAG_ADDR, &flag, 1);

    printf("---- WiFi 凭据已存进 EEPROM:%s ----\r\n", ssid);
    return 1;
}
