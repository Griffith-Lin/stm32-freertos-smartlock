#ifndef __MG200_
#define __MG200_

#include "stm32f4xx.h"

// MG200 指纹模块(USART6 通信):
//   PC0 = DETECT   手指触摸检测输入(下拉,触摸输出高)
//   PC1 = SIG_OUT  模块信号输出控制(推挽)
//   PC6 = USART6_TX → 模块 RXD
//   PC7 = USART6_RX ← 模块 TXD


typedef struct
{
    uint8_t beg;
    uint8_t send_addr;
    uint8_t rec_addr;
    uint8_t cmd;    // 应答码
    uint8_t result; // 返回值
    uint8_t para;   // 参数
    uint8_t reserve;
    uint8_t crc;
    uint8_t valid;  // 起始码、地址和校验和均正确时为 1
} MG_rec_data;

uint8_t usart6_rev_byte(void);

void mg200_Config(uint32_t brr); // 初始化检测脚 + 信号脚 + USART6;brr=与模块约定的波特率

void mg200_send(uint8_t cmd,uint8_t para);

MG_rec_data mg200_rec(void);

#endif
