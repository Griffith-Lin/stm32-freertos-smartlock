#ifndef __MG200_
#define __MG200_

#include "stm32f4xx.h"
#include "stdio.h"
#include "delay.h"


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

void mg200_Config(uint32_t brr); // 初始化检测脚 + 信号脚 + USART6;brr=与模块约定的波特率

void mg200_send(uint8_t cmd,uint8_t para);

MG_rec_data mg200_rec(void);

uint8_t check_rec(MG_rec_data rec);

void mg200_add_finger(void);
void mg200_get_figer_num(void);
void mg200_erase_all(void);
void mg200_compare(void);

/**
 * @brief  带超时的收帧(放进 FreeRTOS 任务里必须用这个,不能用 mg200_rec)
 * @param  rec        输出:收到的应答
 * @param  timeout_ms 从开始找帧头算起的**总**超时,单位 ms
 * @return 1 = 收满了 8 个字节(是否合法看 rec->valid 或 check_rec())
 *         0 = 超时,rec 里的内容不可信
 * @note   mg200_rec() 内部是 while(1) 死等。裸机阶段无所谓,但放进任务后,
 *         模块一旦不应答(没接、没供电、波特率不对),任务就永远卡在那条
 *         语句上出不来。本函数用 TIM10 的 1ms 时基计时,超时立刻返回。
 */
uint8_t mg200_rec_timeout(MG_rec_data *rec, uint32_t timeout_ms);
#endif
