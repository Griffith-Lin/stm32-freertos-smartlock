#include "i2c.h"

#define I2C_SCL PBout(6)
#define I2C_SDA PBout(7)



// pb6(scl，开漏输出) pb7(sda，开漏输出)
void i2c_master_Config(void)
{

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);

    GPIO_InitTypeDef gpio_InitTypeDef = {0};

    // PB6/PB7 的 ODR 复位值是 0，若先配成输出再置 1，SDA=0、SCL=0 会把两条线都拉低，
    // 是从机无法识别的异常状态，要先置 1（释放总线）再切换成输出。
    I2C_SDA = 1;
    I2C_SCL = 1;

    gpio_InitTypeDef.GPIO_Mode  = GPIO_Mode_OUT;
    gpio_InitTypeDef.GPIO_Speed = GPIO_High_Speed;
    gpio_InitTypeDef.GPIO_PuPd  = GPIO_PuPd_NOPULL;

    // SCL(PB6) 用开漏：板上 SCL 有外部上拉电阻，高电平靠它提供。
    // 实测推挽也能跑（总线上只有主机驱动 SCL），但开漏更稳妥：
    // 万一从机拉伸时钟，推挽会和从机对顶，开漏不会。
    gpio_InitTypeDef.GPIO_OType = GPIO_OType_OD;
    gpio_InitTypeDef.GPIO_Pin   = GPIO_Pin_6;
    GPIO_Init(GPIOB, &gpio_InitTypeDef);

    // SDA(PB7) 用开漏：从机要靠拉低 SDA 回应答，推挽会和从机对顶。
    gpio_InitTypeDef.GPIO_OType = GPIO_OType_OD;
    gpio_InitTypeDef.GPIO_Pin   = GPIO_Pin_7;
    GPIO_Init(GPIOB, &gpio_InitTypeDef);

}

/*
I2C 是同步通信，时钟就是"指挥棒"，数据必须在指挥棒落下之前就位，否则接收方无法可靠采样。这和所有同步总线（SPI、并行总线等）的原理完全一致。



SCL = 0（低电平）
数据准备阶段 —— 发送方可以改变 SDA 的值，禁止从机接收
SCL = 1（高电平）
数据有效阶段 —— 告诉从机可以接收，SDA 必须稳定，接收方在此采样读取数据


SDA 和 SCL 在空闲时都是高电平。
所有设备的 SDA/SCL 引脚都是开漏输出（Open-Drain）
开漏输出只能做一件事：把线拉低（输出 0）
没有任何设备拉低时，上拉电阻把线拉到 VCC → 高电平




为什么该用阻塞的方式延时，而不是定时器中断的方式延时？
现阶段（学习期）：
→ 用阻塞 delay_us()，简单可靠，把精力放在理解协议上

进阶（做项目）：
→ 用硬件 I2C 外设 + HAL 库，阻塞或 DMA 都行



起始条件和停止条件是完全对称的设计：起始要求 SDA 从 1→0（所以先确保 SDA=1），
停止要求 SDA 从 0→1（所以先确保 SDA=0）。两者的共同前提是 SCL=1，区别只是 SDA 的跳变方向相反。


主机发 8 bit 数据 → 第 9 个时钟：
从机拉低 SDA (= 0) → ACK  → "收到了，继续发"
从机不拉 SDA   (= 1) → NACK → "没收到 / 别发了 / 我没有这个地址"


*/

// 主机接收从机数据时，主机回应的让从机继续发的应答
void i2c_master_ack(uint8_t ack)
{
    I2C_SCL = 0;
    if (ack)
    {
        I2C_SDA = 0; // ACK，主机告诉从机继续发
    }
    else
    {
        I2C_SDA = 1; // NACK，主机告诉从机停止发
    }
    Delay_Us(4);

    I2C_SCL = 1;
    Delay_Us(4); // 从机在上升沿采样，高电平保持一会儿

    I2C_SCL = 0; // 结束当前ACK/NACK周期： I2C协议规定，数据位（包括ACK位）在SCL高电平期间必须保持稳定。将SCL拉低标志着这个应答位的采样窗口正式关闭。
    Delay_Us(4); // 保证SCL低电平宽度： I2C标准对SCL低电平持续时间有最小值要求（Standard Mode ≥ 4.7μs，Fast Mode ≥ 1.3μs）。这个延时确保即使后续代码立即执行，也不会违反时序。
}

// 从机接收主机数据后，返回的收应答
uint8_t i2c_master_wait_ack(void)
{
    uint8_t ack = 0;

    I2C_SCL = 0; // 在释放 SDA 之前，必须先拉低 SCL，确保处于数据准备阶段。
    I2C_SDA = 1; // sda拉高，数据线空闲状态，不影响引脚输入

    // 这就是软件模拟 I2C 的一个时钟周期。
    // I2C 的时钟全部由主机产生，从机自己没有时钟，必须靠主机在 SCL 上拉低→拉高来"喂"节拍：
    I2C_SCL = 0;
    Delay_Us(4);
    I2C_SCL = 1;

    //返回 1=ACK、0=NACK(CY9CM.c:107-114),和很多驱动库(0=ACK)相反
    if (GPIO_ReadInputDataBit(GPIOB, GPIO_Pin_7))
    {
        ack = 0; // 从机没把sda拉低，从机无应答
    }
    else
    {
        ack = 1;
    }
    Delay_Us(4);

    I2C_SCL = 0;
    Delay_Us(4);
    return ack; // ack==1从机有应答，ack==0从机无应答
}

// 开始条件的定义要求 SDA 从 1→0 跳变
void i2c_master_start(void)
{

    I2C_SDA = 1; // 确保sda拉高
    I2C_SCL = 1; // 拉高时钟线，让从机接收数据线的数据
    Delay_Us(4);
    I2C_SDA = 0; // sda拉低，完成1到0的跳变
    Delay_Us(4);
    I2C_SCL = 0; // 拉低时钟线，衔接下次传输的时钟线状态
}

/*
停止条件的定义要求 SDA 从 0→1 跳变，所以 SDA 必须先处于 0"。
代码中写 SDA=0 是为了确保这个前提条件成立,
不管之前 SDA 是什么状态，都强制拉到 0，然后再在 SCL=1 时释放为 1，产生停止条件
*/
void i2c_master_stop(void)
{
    I2C_SDA = 0; // 确保 SDA = 0，来完成一个从0->1的跳变（可能已经是0了，但保险起见再写一次）
    I2C_SCL = 1;
    Delay_Us(4);
    I2C_SDA = 1; // ← SDA 拉高 → STOP 信号
    Delay_Us(4);
}







/*
协议设计维度：为什么I2C必须高位先行？
很多串行协议（如UART 串口）默认是低位先行（LSB First），为什么I2C 偏偏选择高位先行？这并非随意规定，而是由其硬件仲裁机制决定的。

1.！！！！多主仲裁！！！！！（Multi-MasterArbitration）的必然要求
I2C协议支持"多主模式”（总线上可以有多个单片机同时作为主机）。当两个主机同时尝试控制总线时，
必须通过线"与"逻辑进行仲裁：谁把SDA拉低（发0），谁就赢得总线控制权。·I2C传输的第一个字节通常是从机地址
·因为采用MSBFirst，地址的最高位最先在总线上进行比较。
·结果：地址数值较小的设备，其高位更容易出现0，地址高位先比较，使得地址数值大的设备更早退出。（无损仲裁：发 0 的设备检测到总线为 0（与自己输出一致），继续发送；发 1 的设备检测到总线为 0（与自己输出不符），立即退出竞争，转为从机监听。）
如果采用LSBFirst，则低位决定优先级，完全违背了人类对"数值大小与优先级"的直觉映射，也会让硬件状态机的设计变得极其别扭。
*/

// 发送数据（data为发送的数据）
uint8_t i2c_master_write(uint8_t data)
{
    uint8_t ack = 0;
    for (int i = 0; i < 8; i++) // 依次发8位
    {
        I2C_SCL = 0;
        if (data & (0x80 >> i)) // 从高位发送
        {
            I2C_SDA = 1;
        }
        else
        {
            I2C_SDA = 0;
        }
        Delay_Us(4);
        I2C_SCL = 1;
        Delay_Us(4); // 至此完整一周期
    }
    ack = i2c_master_wait_ack(); // 接收从机的收应答
    return ack;                  //
}

// 接收数据
uint8_t i2c_master_read(uint8_t ack)
{
    uint8_t data = 0;
    I2C_SDA = 1; // sda拉高，数据线空闲不影响引脚输入（根据GPIO图）
    for (int i = 0; i < 8; i++)
    {
        data <<= 1;

        // 这就是软件模拟 I2C 的一个时钟周期。
        // I2C 的时钟全部由主机产生，从机自己没有时钟，必须靠主机在 SCL 上拉低→拉高来"喂"节拍：
        I2C_SCL = 0;
        Delay_Us(4);
        I2C_SCL = 1;

        if (GPIO_ReadInputDataBit(GPIOB, GPIO_Pin_7))
        {
            data |= 1;
        }
        Delay_Us(4);
    }
    i2c_master_ack(ack); // 主机发一位应答，ACK告诉从机继续发，NACK(0)告诉从机停止发
    return data;

    // 主机在每一个时钟沿上都等从机放线,任何停顿(第一帧的 16ms 测量、流中间的湿度补偿停顿)都自然被等待消化,不再依赖运气。
}
