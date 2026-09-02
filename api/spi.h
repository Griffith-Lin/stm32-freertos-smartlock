#ifndef __SPI_
#define __SPI_

#include "stm32f4xx.h"
#include "bitband.h"

void spi1_w25_ini(void);
uint8_t SPI_Echo(u8 data);

void spi1_sd_Init(void);

void spi_simulation_ini(void);
uint8_t SPI_simulation_Echo(uint8_t data);

#define SPI_CS PAout(4)
#define SPI_SCK PAout(5)
#define SPI_MOSI PAout(7)
#define SPI_MISO PAin(6)

#endif

