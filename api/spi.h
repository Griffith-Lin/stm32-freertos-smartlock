#ifndef __SPI_
#define __SPI_

#include "stm32f4xx.h"
#include "bitband.h"

void spi1_w25_Config(void);
uint8_t SPI_Echo(u8 data);

void spi1_sd_Init(void);

void spi_simulation_ini(void);
uint8_t SPI_simulation_Echo(uint8_t data);

#define SPI_CS PAout(4)
#define SPI_SCK PAout(5)
#define SPI_MOSI PAout(7)
#define SPI_MISO PAin(6)

/*----------- SPI3:接 RC522 刷卡模块(PC10/PC11/PC12,复用 AF6)-----------*/
// 🟡 L1 架构代码 —— 只负责"把 SPI3 这个外设配起来"这一件事,不知道对面是谁
// 片选/RST 是 RC522 自己的引脚,配在 rfid.h 里,不往这儿塞。
void spi3_Config(void);
uint8_t SPI3_Echo(uint8_t data);

#endif

