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
 * 🟡 L1 架构代码 —— 任务里唯一能用的收帧入口,选错函数(mg200_rec)任务就永久卡死;timeout_ms 是"找帧头 + 收满 8 字节"的总时限,不是每字节时限
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

/**
 * 🟡 L1 架构代码 —— "先清残字节 → 再发 → 再带超时收"这三步的顺序就是它的全部契约:少了清残字节,上一帧的尾巴会和这一帧拼起来;少了超时,任务就卡死
 * @brief  发一条命令并收一帧应答(任务里发命令统一走这里)
 * @param  cmd        命令码
 * @param  para       参数
 * @param  rec        输出:收到的应答
 * @param  timeout_ms 从开始找帧头算起的**总**超时,单位 ms
 * @return 1 = 收到了一帧(是否合法看 rec->valid 或 check_rec());0 = 超时
 * @note   发之前会先把 USART6 接收缓冲里没收干净的字节丢掉。
 *         上一次没收干净的尾巴(校验失败的帧、上电毛刺)会和这一次的应答拼在
 *         一起,让帧同步从错误的 0x6C 开始 —— 这就是 CLAUDE.md 里记的那个历史 bug,
 *         所以这一步不能省。
 */
uint8_t mg200_cmd(uint8_t cmd, uint8_t para, MG_rec_data *rec, uint32_t timeout_ms);

/**
 * 🔴 L0 生死线代码 —— 不可逆的破坏性命令:一句话就让模块里所有指纹全部消失,而且没有任何确认步骤,风险不在"谁来调"而在这几行本身
 * @brief  清空指纹库(0x54),带超时,可在任务里调用
 * @param  timeout_ms 等应答的总超时(ms)
 * @return 1 = 成功;0 = 超时 / 帧非法 / 模块报错
 * @note   原来的 mg200_erase_all() 内部走的是**无超时**的 mg200_rec(),
 *         放进任务里模块一旦不应答就永久卡死,所以任务里必须用这个版本。
 * @note   超时给 5000 左右:模块清空整个库比回一条普通命令慢。
 */
uint8_t mg200_erase_all_timeout(uint32_t timeout_ms);
#endif
