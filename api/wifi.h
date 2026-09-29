#ifndef __WIFI_
#define __WIFI_

#include "stm32f4xx.h"

// ── Ai-Thinker WB2(Combo AT 固件):STM32 与分包平台之间的 MQTT 通道 ──
//
// 接线(注意 TX/RX 要交叉,而且必须共地):
//
//     STM32 PA2 (USART2_TX, AF7)  ──→  WB2 的 RX
//     STM32 PA3 (USART2_RX, AF7)  ←──  WB2 的 TX
//     GND                          ───  GND
//
// 模块出厂波特率 115200,AT 命令一律以 "\r\n" 结尾。
//
// 分工:WB2 自己跑 TCP/TLS/ MQTT 协议栈,STM32 只用 AT 命令遥控它。
//       所以这里没有 LwIP、没有 socket,只有一串字符串收发。

#define WIFI_UART_BAUD      115200      // WB2 默认波特率

/**
 * 🔴 L0 生死线代码 —— 里子全在 wifi.c 的同名函数:PA2/PA3 的 AF7 复用号、USART2 挂 APB1、NVIC 抢占优先级 3、只开 RXNE,四处都得对着手册和原理图核;而且必须赶在 vTaskStartScheduler() 之前调完
 * @brief  初始化 USART2(PA2/PA3)—— 只配硬件,不发命令
 * @return 无
 * @note   **必须在 vTaskStartScheduler() 之前调用**。
 *         真正的联网动作在 wifi_task 里做,因为连 WiFi 要等好几秒,
 *         裸机阶段忙等会把启动流程卡住。
 */
void wb2_Config(void);

/**
 * 🟡 L1 架构代码 —— 要拿捏的只有两件事:优先级 1(低于所有业务任务,联网再慢也抢不走开锁的 CPU)和 512 字栈(按最深调用链 + printf 开销估的,给少了就是静默的栈溢出)
 * @brief  创建 WiFi/MQTT 任务
 * @return 无
 * @note   在 vTaskStartScheduler() 之前调用。
 *         任务优先级 1,比所有业务任务(2/3)都低 —— 联网慢一点没关系,
 *         但绝不能让它抢了开锁、按键、指纹的 CPU。
 */
void wifi_tasks_init(void);

/**
 * 🟡 L1 架构代码 —— "投递意图而非执行"这条分工的入口:它只置一个标志就返回,配网本身由 wifi_task 做;AT 命令和 USART2 的环形缓冲都只归那一个任务,谁都能发命令的话两边的超时会互相把对方的应吃掉
 * @brief  请求开一次蓝牙配网(管理员模式 555# 用),**非阻塞**
 * @return 无
 * @note   **只能在任务里调**。admin 跑在 lock_task 里,所以这里是跨任务投递,
 *         真正的动作由 wifi_task 执行 —— USART2 的收发和那条 AT 状态机
 *         只归 wifi_task 一家,别的任务直接发 AT 命令会把它的读行打乱。
 * @note   配网建议是阻塞的(要等用户掏手机),所以这里绝不能同步等 ——
 *         那会把 lock_task 卡住,锁的 tick 一停就不回锁了。
 * @note   已经在配网时这次请求会被丢掉并打印一行日志:配网期间那串 AT 命令
 *         不能被新的请求打断(最要紧的是收尾那条 AT+WCONFIG=0,被截了就
 *         等于把"谁都能配网"的口子一直开着)。
 */
void wifi_request_provision(void);

#endif // !__WIFI_
