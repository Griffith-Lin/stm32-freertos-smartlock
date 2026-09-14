
#ifndef __I2C_
#define __I2C_

#include "stm32f4xx.h"

#include "delay.h"

#include "bitband.h"

void i2c_master_Config(void);

void i2c_master_ack(uint8_t ack);
uint8_t i2c_master_wait_ack(void);

void i2c_master_start(void);
void i2c_master_stop(void);

uint8_t i2c_master_write(uint8_t data);
uint8_t i2c_master_read(uint8_t ack);



#endif
