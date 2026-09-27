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

#endif
