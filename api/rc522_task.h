#ifndef __RC522_TASK_
#define __RC522_TASK_

#include "stm32f4xx.h"

// ══════════════════════════════════════════════════════════════════════════
//  刷卡开锁 —— RC522(MF522)业务层
//
//  三层分工(和指纹那条链完全对应):
//      硬件层  spi.c 的 spi3_Config() / SPI3_Echo()   PC10/PC11/PC12(AF6)
//      协议层  rfid.c / rfid.h                        官方移植的寻卡/防冲突/读写块
//      业务层  本文件                                 寻到卡 → 比对卡号 → 开锁
//                                                     (相当于 finger.c 在 MG200 那边的角色)
//
//  "谁有资格开锁"这件事的答案存在 AT24C04A EEPROM 里(掉电不丢),
//  布局见 rc522_task.c 顶部。**第一张刷上去的卡会自动被录成授权卡**,
//  之后只认它 —— 和 lock 的密码"首次上电写入默认值"是同一套做法。
// ══════════════════════════════════════════════════════════════════════════

/**
 * 🟡 L1 架构代码 —— 生命周期约束:内含 RC522 硬复位和一次 EEPROM 读(都是裸机忙等),必须赶在调度器启动前调用
 * @brief  刷卡模块初始化:配好 RC522,再从 EEPROM 载入已授权的卡号
 * @return 无
 * @note   **必须在 vTaskStartScheduler() 之前调用**,理由和 lock_init() 一样:
 *         里面走的是 Delay_Us / I2C 那种忙等延时,调度器起来之后调会把别的
 *         任务饿死。
 * @note   依赖 i2c_bus_config(&i2c_bus_at24c04) 已经把 EEPROM 那条 I2C 总线配好。
 */
void rc522_init(void);

/**
 * 🟡 L1 架构代码 —— 创建时机的依赖写在接口上:要等 lock_tasks_init() 之后,否则刷卡通过了也没有任务去驱动电机
 * @brief  创建刷卡任务
 * @return 无
 * @note   刷卡走的是 lock.c 的公共开锁流程(lock_open / lock_is_busy),
 *         所以要在 lock_tasks_init() 之后调用。
 */
void rc522_tasks_init(void);

#endif
