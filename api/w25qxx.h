#ifndef _W25QXX_H_
#define _W25QXX_H_

#include "stm32f4xx.h"
#include "spi.h"
#include "bitband.h"
#include "stdio.h"
#include <stdint.h> 

void W25Qxx_Read_ID(void);

uint8_t W25Qxx_Read_SR1_busy(void);

void W25Qxx_write_enable(void);
void W25Qxx_write_disable(void);

void W25Qxx_Lock(void);

void W25Qxx_write_SR1(void);

void W25Qxx_Sector_Erase(uint8_t sector);
void W25Qxx_Block_Erase(u32 addr);
void W25Qxx_Chip_Erase(void);

int8_t W25Qxx_Page_Program(uint32_t addr, const uint8_t *data, uint32_t len);
void W25Qxx_CrossPage(uint32_t addr, const uint8_t *buff, uint32_t len);

int8_t W25Qxx_Read_Bytes(uint32_t addr, uint8_t *data, uint32_t len);

void W25Qxx_simulation_Read_ID(void);
#endif
