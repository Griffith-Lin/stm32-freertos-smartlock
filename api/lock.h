#ifndef __LOCK_
#define __LOCK_

#include "stm32f4xx.h"

// 密码锁业务逻辑
//   输入设备:电容触摸板(CY9CMBR3116),按键经 cy9cm_key_queue 送到这里
//   密码存储:AT24C04A EEPROM(掉电不丢),首次上电自动写入默认密码
//   输出:      LCD 显示输入界面;密码正确后驱动 L9110S 电机动作出锁
//
// 按键约定(4x3 键盘):
//   '0'~'9'  输入密码,屏幕上一个 '-'
//   '*'      清空已输入的,重新输
//   '#'      确认,校验密码

#define LOCK_PWD_LEN    6       // 密码位数
#define LOCK_OPEN_MS    3000    // 开锁后保持多久自动回锁(ms)
#define LOCK_MOTOR_MS   500     // 开锁/回锁的电机行程时间(ms),按实际锁体调

/**
 * @brief  密码锁初始化:从 EEPROM 加载密码(没有就写默认值)+ 画开机界面
 * @return 无
 * @note   **必须在 vTaskStartScheduler() 之前调用**。
 *         里面会走一次 EEPROM 写(首次上电时),而 AT24C04_page_write() 用的是
 *         Delay_Ms() 忙等——裸机阶段无所谓,调度器起来之后会把别的任务饿死。
 *         加载完之后密码缓存在 RAM 里,任务阶段不再碰 EEPROM。
 */
void lock_init(void);

/**
 * @brief  创建密码处理任务
 * @return 无
 * @note   在 vTaskStartScheduler() 之前调用。任务阻塞在 cy9cm_key_queue 上,
 *         没按键时不占 CPU。依赖 cy9cm_tasks_init() 已经建好队列。
 */
void lock_tasks_init(void);

/**
 * @brief  开锁 —— 所有开锁来源的公共出口(密码、指纹……都走这里)
 * @param  title 开锁期间标题行显示的 ASCII 文字
 * @param  color 标题颜色
 * @return 1 = 本次真的开了锁;0 = 已有开锁流程在进行,本次请求被忽略
 * @note   **互斥**:同一时刻只允许一个开锁流程,免得两个任务同时去推电机。
 *         "检查 + 占用标志"这两步放在临界区里做 —— 中间只要被 SysTick 切走
 *         一次,两个任务就可能都读到"没人开锁",然后一起去驱动 L9110S。
 * @note   整个流程阻塞 LOCK_MOTOR_MS×2 + LOCK_OPEN_MS 毫秒,内部用
 *         vTaskDelay 让出 CPU。**只能在任务里调用,不能在中断里调。**
 */
uint8_t lock_open(uint8_t *title, uint16_t color);

/**
 * @brief  只改标题行,不动锁(给指纹那种模块显示"没匹配上"之类的提示用)
 * @param  title ASCII 文字,**至少 8 个字符**
 * @param  color 颜色
 * @note   为什么要求 8 个字符:LCD_Font_Dis_Ascii() 会把每个字符格子的每个
 *         像素都写一遍(笔画写前景色、其余写背景色),所以只有"新文字不比旧
 *         文字短"才盖得干净。标题行原本是 "PASSWORD" 8 个字符,传个短的
 *         过去,右边会留下上一行标题的残影 —— 短了就用空格补齐。
 */
void lock_lcd_set_title(uint8_t *title, uint16_t color);

/**
 * @brief  是否正在开锁/回锁
 * @return 1 = 正在进行
 * @note   给别的开锁来源做"预检",省得白采一次指纹再被 lock_open() 拒掉。
 *         真正的仲裁仍然是 lock_open() 自己,**不能只靠这个判断**。
 */
uint8_t lock_is_busy(void);

#endif
