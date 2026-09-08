#include "mcu_wake.h"

//mcu_wkup PA0,空闲状态下输入的是低电平
//配置为上拉输入模式
/**
 * @brief 待机唤醒模块初始化:开 PWR 时钟 + 配 PA0
 * @return 无
 * @note PWR_GetFlagStatus/PWR_ClearFlag 等都要先开 PWR 时钟(APB1),须最先调用
 */
void mcu_wkup_Config(void)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE); // 访问 PWR 寄存器前必须开时钟

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);

    GPIO_InitTypeDef gpio_InitTypeDef = {0};

    gpio_InitTypeDef.GPIO_Mode = GPIO_Mode_IN;
    gpio_InitTypeDef.GPIO_Speed = GPIO_Low_Speed;
    gpio_InitTypeDef.GPIO_PuPd = GPIO_PuPd_NOPULL; // 空闲低靠外部下拉;内部上拉会把 PA0 拉高,一进待机就误唤醒
    gpio_InitTypeDef.GPIO_Pin = GPIO_Pin_0;

    GPIO_Init(GPIOA, &gpio_InitTypeDef);
}

/**
 * @brief 检测本次复位是否来自 PA0 待机唤醒
 * @return 1 = 待机唤醒复位;0 = 冷启动/普通复位
 * @note 在 mcu_wkup_Config() 之后尽早调用;内部会清掉 WUF 标志
 */
uint8_t mcu_was_standby_wake(void)
{
    uint8_t wake = (PWR_GetFlagStatus(PWR_FLAG_WU) != RESET); // 读 PWR->CSR 的 bit0，待机唤醒会置位 WUF。WUF 的特殊之处:它不在 CPU 内部,挂在电源控制域——复位不会清零它。于是它是唤醒时唯一记得"昨晚发生了什么"的寄存器。冷启动上电:WUF = 0
    PWR_ClearFlag(PWR_FLAG_WU);                               // 向 PWR->CR 的 CWUF 写 1,把 WUF 清回 0。否则下次冷启动也会误判成唤醒
    return wake;
}

/**
 * @brief 进入 STANDBY 模式(长按 PC13 业务调用)
 * @return 无(正常情况永不返回)
 * @note 唤醒方式:PA0 上升沿(WKUP 脚) → 整片复位,代码从头执行。
 *       进待机前先等串口把最后一个字节发完,否则复位会截断尾巴。
 *       调试注意:进 STANDBY 后内核断电,调试器会掉线 —— 属正常现象,
 *       按 PA0 唤醒复位后用 eIDE 重新连接/下载即可。
 */
void mcu_enter_standby(void)
{
    while ((USART1->SR & (1 << 6)) == 0) // TC 位:等最后一个字节完全发完
        ;

    PWR_WakeUpPinCmd(ENABLE);   // EWUP=1:允许 PA0 上升沿唤醒(必须在进待机前置位)
    PWR_ClearFlag(PWR_FLAG_WU); // 清残留唤醒标志,保证睡下是干净的

    PWR_EnterSTANDBYMode();     // 内部:置 PDDS + SLEEPDEEP 后执行 WFI → 睡死
    //   1. PWR->CR |= CWUF            —— 再清一次标志
    //   2. PWR->CR |= PDDS            —— 关键!决定"待机"而不是"停机"
    //   3. SCB->SCR |= SLEEPDEEP      —— 注意这是 Cortex 内核的位,不是 PWR 外设
    //   4. __WFI()                    —— 执行完这条,内核断电
    
    for (;;) // 防御:走到这 = 配置有误,没真正睡下
        ;
}
