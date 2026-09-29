#ifndef __FINGER_
#define __FINGER_

#include "stm32f4xx.h"

// 指纹开锁业务
//   输入:MG200 指纹模块(USART6)。手指按上去时 PC0(DETECT) 输出高电平。
//   输出:比对通过后走 lock.c 的 lock_open() 驱动 L9110S 电机出锁。
//
// 和密码开锁的关系:
//   两条开锁路径共用同一个 lock_open(),由它保证同一时刻只有一个开锁动作,
//   不会出现"密码任务和指纹任务同时去推电机"。指纹这边额外做一次
//   lock_is_busy() 预检,只是为了省得白采一次指纹再被拒掉。
//
// 为什么用轮询而不是中断:
//   MG200.c 的协议层是轮询收发(usart6_rev_byte 直接等 RXNE),所以 USART6
//   的接收中断不能开 —— 中断会先把 RXNE 读走,轮询就永远等不到数据。
//   反过来,任务里也**不能用 mg200_rec()**:它是 while(1) 死等,模块一旦
//   不应答(没接、没供电、波特率不对),任务就永远卡在那条语句上。
//   这里统一用带超时的 mg200_rec_timeout()。

/**
 * 🟡 L1 架构代码 —— 任务栈(256 字)和优先级(2)从这里定下去,而堆只剩约 1.2KB:改这个数字必须连 heap_4 的余量一起算,否则失败现象是 malloc 失败钩子死循环
 * @brief  创建指纹开锁任务
 * @return 无
 * @note   在 vTaskStartScheduler() 之前调用。
 *         任务以 100ms 周期轮询 PC0,没手指时只读一个 GPIO,几乎不占 CPU。
 */
void finger_tasks_init(void);

#endif
