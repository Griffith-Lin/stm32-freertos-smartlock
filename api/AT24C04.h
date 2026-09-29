#ifndef __AT24C04__
#define __AT24C04__

#include "stm32f4xx.h"
#include "i2c.h"
#include "stdio.h"

// AT24C04A:4Kbit = 512 字节 I2C EEPROM,挂在 i2c.c 那条 bit-bang 总线上
//   器件地址 1010 A2 A1 P0 R/W,其中 A2/A1 是板上的硬件地址脚,
//   P0 不是地址脚 —— 它是第 9 位存储地址,由驱动内部按 addr 自动算好。
//   地址 0~255 → 0xA0/0xA1;地址 256~511 → 0xA2/0xA3。

/**
 * 🟡 L1 架构代码 —— 接口契约是"本函数不跨页":len 不超过页内剩余空间这条前提整个推给调用方,传超了硬件静默回卷,函数只回一个应答状态码
 * @brief  向 EEPROM 写一页以内的数据(不跨页)
 * @param  addr 起始存储地址(0~511)
 * @param  arr  待写入的数据
 * @param  len  写入字节数,必须 <= 本页剩余空间
 * @return 0=成功;1=器件地址无应答;2=字地址无应答;3=数据无应答
 */
uint8_t AT24C04_page_write(uint16_t addr, uint8_t *arr, uint16_t len);

/**
 * 🟡 L1 架构代码 —— 假写定地址、重复 START、末字节 NACK 这些总线上的讲究全封在实现里,对外只留"给地址给缓冲区"一层,接口看不出底下有这些步骤
 * @brief  从指定地址连续读出若干字节
 * @param  addr 起始存储地址(0~511)
 * @param  buf  接收缓冲区
 * @param  len  读取字节数
 * @return 0=成功;1=器件地址无应答;2=字地址无应答
 */
uint8_t AT24C04_read(uint16_t addr, uint8_t *buf, uint16_t len);

/**
 * 🟡 L1 架构代码 —— 对上层只暴露"任意起点、任意长度",页边界如何拆是实现的自由;代价是返回 void,失败信号只有串口打印这一条通道
 * @brief  写任意长度数据,自动按 16 字节页边界拆成多次页写
 * @param  addr 起始存储地址(0~511)
 * @param  arr  待写入的数据
 * @param  len  写入字节数
 * @return 无(失败时打印出是哪一步没应答)
 */
void AT24C04_crosspage_write(uint16_t addr, uint8_t *arr, uint16_t len);

#endif
