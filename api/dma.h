#ifndef __DMA_
#define __DMA_

#include "stm32f4xx.h"
#include "w25qxx.h"


#define BUFFSIZE 4096

enum
{
    Free_IT,
    DMA_IT,
    Usart_IT
}; // 0:Free_IT(空闲)/   1:DMA_IT(收满一块)/   2:Usart_IT(文件结束)

typedef struct
{
    u8 buff[2][BUFFSIZE]; // 双缓冲本体
    u8 sta;               // 状态标志,Free_IT(空闲)/ DMA_IT(收满一块)/ Usart_IT(文件结束)
    u32 addr;             // 当前写 flash 的地址
    u32 len;              // 本次数据长度
    u8 flag;              // 该写哪块缓冲
} Font;

extern Font font;


void DMA_Font_Config(void);
void Font_Load(void);
#endif

