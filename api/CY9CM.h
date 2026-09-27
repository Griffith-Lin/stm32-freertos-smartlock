#ifndef __CY9CM_
#define __CY9CM_

#include "stm32f4xx.h"
#include "i2c.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

// CY8CMBR3116 电容触摸按键(4x3 密码键盘)控制器
//   I2C 从机地址 0x37(7 位) → 写字节 0x6E / 读字节 0x6F
//   RES  PA1   低电平有效脉冲复位
//   SCL  PB6
//   SDA  PB7

#define CY9CM_ADDR7   0x37
#define CY9CM_ADDR_W  ((CY9CM_ADDR7 << 1) | 0)   // 0x6E 写
#define CY9CM_ADDR_R  ((CY9CM_ADDR7 << 1) | 1)   // 0x6F 读

#define CY9CM_KEY_NONE  0    // 无按键时的返回值

void     CY9CM_Config(void);                                             // 只配 RES 脚
void     CY9CM_Init(void);                                               // 完整初始化:RES 脚 + 复位脉冲 +(必要时)写配置
uint8_t  CY9CM_WriteData(uint8_t reg, uint8_t len, const uint8_t *data); // 从 reg 起连续写 len 字节
uint8_t  CY9CM_ReadData(uint8_t reg, uint8_t len, uint8_t *data);        // 从 reg 起连续读 len 字节
void     CY9CM_RegConfig(uint8_t set_mode);                              // 写 128 字节配置
uint8_t  CY9CM_ReadKey(void);                                            // 读当前按键,无键返回 CY9CM_KEY_NONE
uint8_t  CY9CM_KeyScan(void);                                            // 带边沿检测的按键扫描
void cy9cm_tasks_init(void);

// 触摸板按键事件队列:cy9cm_scan 任务生产,lock 任务消费
// (驱动只负责产生"发生了什么",由谁来处理是业务层的事)。
// 事件内容就是按键字符 '0'~'9' / '*' / '#'。
extern QueueHandle_t cy9cm_key_queue;

#endif
