
#ifndef __I2C_
#define __I2C_

#include "stm32f4xx.h"

#include "delay.h"

#include "bitband.h"

// ── 总线描述 ────────────────────────────────────────────────────────────────
// I2C 的时序(起始/停止/应答/移位)和挂的是什么器件无关,只有"用哪两个引脚"
// 是硬件相关的。把这部分信息装进一个结构体,同一套时序代码就能服务多条总线:
// 每条总线在 i2c.c 里定义成一个 i2c_bus_t 常量,调用时把它的地址传进去。
//
// 为什么不搞个"当前总线"全局变量省掉参数?因为 CY9CM 和 AT24C04 分属不同任务,
// 全局变量会让两个任务互相踩,而且看代码时不知道这句操作的是哪条总线。
typedef struct
{
    GPIO_TypeDef *scl_port;  // SCL 所在端口
    uint16_t      scl_pin;   // SCL 引脚号(GPIO_Pin_x)
    GPIO_TypeDef *sda_port;  // SDA 所在端口
    uint16_t      sda_pin;   // SDA 引脚号
    uint32_t      rcc_ahb1;  // 这两个端口所属的 AHB1 时钟使能位
} i2c_bus_t;

extern const i2c_bus_t i2c_bus_cy9cm;    // CY8CMBR3116 电容按键:SCL=PB6 SDA=PB7
extern const i2c_bus_t i2c_bus_at24c04;  // AT24C04A EEPROM   :SCL=PA8 SDA=PC9

/**
 * @brief  初始化一条 bit-bang I2C 总线上的 SCL/SDA 两个引脚
 * @param  bus 总线描述,传上面两个常量的地址
 * @return 无
 * @note   必须在任何 i2c_master_xxx 之前调用;同一总线重复调用无副作用。
 */
void i2c_bus_config(const i2c_bus_t *bus);

// 下面这些函数都要显式传 bus,别再写成不带参数的老样子
void i2c_master_ack(const i2c_bus_t *bus, uint8_t ack);
uint8_t i2c_master_wait_ack(const i2c_bus_t *bus);

void i2c_master_start(const i2c_bus_t *bus);
void i2c_master_stop(const i2c_bus_t *bus);

uint8_t i2c_master_write(const i2c_bus_t *bus, uint8_t data);
uint8_t i2c_master_read(const i2c_bus_t *bus, uint8_t ack);



#endif
