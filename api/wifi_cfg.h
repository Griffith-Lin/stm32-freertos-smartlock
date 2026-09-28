#ifndef __WIFI_CFG_
#define __WIFI_CFG_

#include "stm32f4xx.h"

// ── WiFi 名字/密码在 AT24C04A EEPROM 里的存取 ──
//
// 为什么要存:原来 SSID 和密码是**硬编码在 wifi.c 里**的,改一次网就得重新编译
// 烧录,而且用户蓝牙配网配好的网,下次上电固件根本不认识 —— 配了等于白配。
// 存进 EEPROM 之后,开机读出来照着连就行,配网配的也是同一份,两边永远一致。
//
// 存储布局(EEPROM 共 512 字节,地址 0~7 被锁密码占了,这里从 8 开始):
//
//     地址 8 ~ 40     SSID   33 字节定长,内容后面补 0
//     地址 41 ~ 104   密码   64 字节定长,内容后面补 0
//     地址 105        标志   0xA6 = 前面两个字段有效
//
// 定长而不是"长度 + 内容":读的时候不用先算长度,字段位置是死的,少一层出错的地方;
// 代价是每次写要写满 98 字节(7 个 EEPROM 页)。配网是极低频操作,这点代价无所谓。
//
// ⚠ 标志字节**故意放在整块的末尾**,而且写的时候**最后一步才落** ——
//   见 wifi_cfg_save() 里的说明,这是为了"写到一半掉电"不会读出一半新一半旧的凭据。

#define WIFI_CFG_SSID_MAX   32      // 802.11 规定 SSID 最长 32 字节
#define WIFI_CFG_PWD_MAX    63      // WPA2 密码最长 63 个 ASCII 字符

/**
 * @brief  从 EEPROM 读 WiFi 凭据
 * @param  ssid 输出:WiFi 名,缓冲至少 WIFI_CFG_SSID_MAX + 1 字节
 * @param  pwd  输出:密码,  缓冲至少 WIFI_CFG_PWD_MAX  + 1 字节
 * @return 1 = 读到了有效凭据(ssid/pwd 已填好);0 = 没存过 / 读失败 / 内容不合法
 * @note   返回 0 时不会去动 ssid/pwd 的内容,调用者别拿它们当有效值用。
 * @note   **只能在任务上下文调用**:底层软件 I2C 逐位翻转电平,读 97 字节要几毫秒,
 *         全程忙等。裸机阶段(lock_init 那种)调也可以,但别在中断里调。
 */
uint8_t wifi_cfg_load(char *ssid, char *pwd);

/**
 * @brief  把 WiFi 凭据写进 EEPROM(蓝牙配网成功后调用)
 * @param  ssid WiFi 名,长度 1 ~ WIFI_CFG_SSID_MAX
 * @param  pwd  密码,  长度 0 ~ WIFI_CFG_PWD_MAX(开放热点没有密码)
 * @return 1 = 写进去了;0 = 长度不合法或总线失败,一个字都没写
 * @note   写一次要等 9 个 EEPROM 内部写周期(tWR),约 90ms 忙等。
 *         只被 wifi_task 调用(优先级 1,最低),而且只在配网成功那一刻发生一次,
 *         对业务任务没有影响。
 */
uint8_t wifi_cfg_save(const char *ssid, const char *pwd);

#endif // !__WIFI_CFG_
