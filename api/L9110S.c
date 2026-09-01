#include "L9110S.h"
#define MOTOR_FOR_PIN   GPIO_Pin_3   // 接 L9110S 的 FOR 输入
#define MOTOR_BAK_PIN   GPIO_Pin_4   // 接 L9110S 的 BAK 输入


// X4  X3负责直接驱动电机，外接引脚
// BAK FOR负责方向 pb3 pb4
// 当BAK FOR都为0时，x4 x3高阻态
void L9110S_ini(void)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);

    GPIO_InitTypeDef gpio_InitTypeDef = {0};

    gpio_InitTypeDef.GPIO_Mode = GPIO_Mode_OUT;
    gpio_InitTypeDef.GPIO_OType = GPIO_OType_PP;
    gpio_InitTypeDef.GPIO_Speed = GPIO_Low_Speed;
    gpio_InitTypeDef.GPIO_PuPd = GPIO_PuPd_NOPULL;
    gpio_InitTypeDef.GPIO_Pin = GPIO_Pin_3 | GPIO_Pin_4;

    GPIO_Init(GPIOB, &gpio_InitTypeDef);
}



void L9110S_control(L9110S_Mode_t mode)
{
    switch (mode)
    {
    case MOTOR_FORWARD:                       // FOR=1 BAK=0 → 正转
        GPIO_SetBits(GPIOB, MOTOR_FOR_PIN);
        GPIO_ResetBits(GPIOB, MOTOR_BAK_PIN);
        break;
    case MOTOR_REVERSE:                       // FOR=0 BAK=1 → 反转
        GPIO_ResetBits(GPIOB, MOTOR_FOR_PIN);
        GPIO_SetBits(GPIOB, MOTOR_BAK_PIN);
        break;
    case MOTOR_BRAKE:                         // FOR=1 BAK=1 → X3=X4=0 短接刹车
        GPIO_SetBits(GPIOB, MOTOR_FOR_PIN | MOTOR_BAK_PIN);
        break;
    case MOTOR_FREE:                          // FOR=0 BAK=0 → 输出高阻
    default:
        GPIO_ResetBits(GPIOB, MOTOR_FOR_PIN | MOTOR_BAK_PIN);
        break;
    }
}

void L9110S_move(L9110S_Mode_t mode, uint32_t ms)
{
    uint32_t start = GetTim10Tick();
    L9110S_control(mode);
    while (GetTim10Tick() - start < ms);      // 走 ms 毫秒
    L9110S_control(MOTOR_BRAKE);              // 到位后刹车，锁舌不会惯性溜回
}
