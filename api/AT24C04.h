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
 * @brief  向 EEPROM 写一页以内的数据(不跨页)
 * @param  addr 起始存储地址(0~511)
 * @param  arr  待写入的数据
 * @param  len  写入字节数,必须 <= 本页剩余空间
 * @return 0=成功;1=器件地址无应答;2=字地址无应答;3=数据无应答
 */
uint8_t AT24C04_page_write(uint16_t addr, uint8_t *arr, uint16_t len);

/**
 * @brief  从指定地址连续读出若干字节
 * @param  addr 起始存储地址(0~511)
 * @param  buf  接收缓冲区
 * @param  len  读取字节数
 * @return 0=成功;1=器件地址无应答;2=字地址无应答
 */
uint8_t AT24C04_read(uint16_t addr, uint8_t *buf, uint16_t len);

/**
 * @brief  写任意长度数据,自动按 16 字节页边界拆成多次页写
 * @param  addr 起始存储地址(0~511)
 * @param  arr  待写入的数据
 * @param  len  写入字节数
 * @return 无(失败时打印出是哪一步没应答)
 */
void AT24C04_crosspage_write(uint16_t addr, uint8_t *arr, uint16_t len);

#endif
