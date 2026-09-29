#ifndef __NV400_
#define __NV400_

#include "stm32f4xx.h"

// ── NV400F 单线 PWM 语音模块 ──
//   PC5 -> VOICE_DATA   推挽输出,MCU 发给模块的命令线
//   PC4 <- VOICE_BUSY   播放忙信号(当前没用到,先配成输入留着)
//
// 协议:空闲高电平;起始 = 拉低 3~9ms(本驱动取 5ms);
//       然后 8 位数据、低位先出,每位 = 一个高电平 + 一个低电平,
//       靠两者的宽度比区分:"1" = 高1200us + 低400us(3:1)
//                           "0" = 高400us  + 低1200us(1:3)
//       发完拉回高电平。
// 命令:00H~DFH = 播放第 1~223 段;F2H 循环播放;FEH 停止。
//
// 注意:芯片的控制方式(一线串口 / 二线串口 / 按键 / UART / 脉冲数)是跟着
//      音频一起配置进芯片的,**同一时刻只有一种生效**。如果烧录时选的不是
//      "一线串口",这里发什么都没反应 —— 这是"烧录成功却不出声"的头号原因。
#define VOICE_DATA_H GPIO_SetBits(GPIOC,GPIO_Pin_5)
#define VOICE_DATA_L GPIO_ResetBits(GPIOC,GPIO_Pin_5)

void NV400F_Init(void);

/**
 * 🟡 L1 架构代码 —— 协议全在这条命令里:低位先出、每一位用高低电平的宽度比(3:1 / 1:3)区分 0 和 1,参数只能对着手册核;一次 18ms 的忙等决定了它只能用在调度器启动前
 * @brief  发一条 8 位命令
 * @param  cmd 命令码 / 语音地址,低位先出
 * @return 无
 * @note   内部是精确忙等,一条命令耗时约 18ms。
 *         只能在调度器启动前、或对实时性没要求的场合调用;
 *         放进 FreeRTOS 任务里会把 CPU 整整占死这 18ms。
 */
void NV400F_SendCmd(u8 cmd);

/**
 * 🟡 L1 架构代码 —— 不是普通延时:改成读 TIM10 硬件计数器就是为了让码宽符合手册(旧的 Delay_Us 按 168MHz 标定,84MHz 上实际延时翻倍);"调用前 TIM10 必须已在跑"是它的硬前提
 * @brief  微秒级忙等延时(用 TIM10 的硬件计数器计时)
 * @param  us 延时多少微秒
 * @return 无
 * @note   调用前 TIM10 必须已经初始化 —— TIM10_Task_Config(84,1000) 会把
 *         CNT 配成 1us 一跳。同样是忙等,只能在调度器启动前用。
 */
void NV400F_DelayUs(uint32_t us);

#endif // !__NV400_
