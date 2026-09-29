#include "i2c.h"

// ── 本文件支持的总线 ────────────────────────────────────────────────────────
// 一条总线就是一对引脚。加新器件时只要在下面加一个常量,时序代码一行都不用动。

// CY8CMBR3116 电容触摸按键:SCL=PB6  SDA=PB7(同端口)
const i2c_bus_t i2c_bus_cy9cm =
{
    GPIOB, GPIO_Pin_6,
    GPIOB, GPIO_Pin_7,
    RCC_AHB1Periph_GPIOB
};

// AT24C04A EEPROM:SCL=PA8  SDA=PC9(两条线跨端口,所以时钟要开两个)
const i2c_bus_t i2c_bus_at24c04 =
{
    GPIOA, GPIO_Pin_8,
    GPIOC, GPIO_Pin_9,
    RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOC
};


// 🔴 L0 生死线代码 —— 直接写端口 BSRR:低 16 位置位、高 16 位复位,掩码或移位方向算错就是拉错引脚,总线当场锁死
// ── 引脚读写 ────────────────────────────────────────────────────────────────
// 原来用 bit-band 宏 PBout(6) 直接写某一位,但 bit-band 别名地址是编译期常量,
// 引脚一旦变成运行时才知道的参数就用不了了,所以改用 BSRR / IDR 这两个寄存器:
//   BSRR:低 16 位写 1 = 置位,高 16 位写 1 = 复位。一条指令原子完成,不用读-改-写。
//   IDR :读引脚的真实电平(开漏输出时读到的是总线上的电平,不是 ODR 里写的值)。
static void i2c_scl_write(const i2c_bus_t *bus, uint8_t v)
{
    bus->scl_port->BSRR = v ? bus->scl_pin : ((uint32_t)bus->scl_pin << 16);
}



// 🔴 L0 生死线代码 —— 与 SCL 同款的 BSRR 原子写,掩码错一位就翻到同一端口别的引脚上,而且没有任何断言会发现
static void i2c_sda_write(const i2c_bus_t *bus, uint8_t v)
{
    bus->sda_port->BSRR = v ? bus->sda_pin : ((uint32_t)bus->sda_pin << 16);
}

/*
相当于
static void i2c_sda_write(const i2c_bus_t *bus, uint8_t v)
{
    if (v)   // v 只是条件
    {
        bus->sda_port->BSRR = bus->sda_pin;                    // 写入 0x00000200  ，标准库中#define GPIO_Pin_9    ((uint16_t)0x0200) ，在低16位对应位置置位
    }
    else
    {
        bus->sda_port->BSRR = ((uint32_t)bus->sda_pin) << 16;  // 写入 0x02000000
    }
}

*/

// 🔴 L0 生死线代码 —— 用 IDR & 掩码采引脚真实电平,掩码写错时 ACK 判断就退化成常数,总线看着"通"其实每个字节都在自说自话
static uint8_t i2c_sda_read(const i2c_bus_t *bus)
{
    return (bus->sda_port->IDR & bus->sda_pin) ? 1 : 0;
}
/*
① bus->sda_port->IDR —— 顺着端口指针取 IDR(Input Data Register)成员。它在结构体里的偏移是 0x10,所以对 AT24C04 来说地址是 0x40020800 + 0x10 = 0x40020810。

它返回整个端口的 16 个引脚电平,每一位对应一根脚。

② & bus->sda_pin —— 按位与,把不关心的位全部清 0,只留下 SDA 那一位。

③ ? 1 : 0 —— 把结果归一到恰好 1 或 0
*/


/**
 * 🔴 L0 生死线代码 —— 上电复位时 ODR=0,必须先写 1 再切成输出,顺序反了就一上电把 SCL/SDA 按在低电平;开漏/推挽选错还会和拉伸时钟的从机对顶
 * @brief  初始化一条 bit-bang I2C 总线上的 SCL/SDA 两个引脚
 * @param  bus 总线描述(i2c_bus_cy9cm / i2c_bus_at24c04)
 * @return 无
 * @note   两条线都配成开漏输出。板上 SCL/SDA 都有外部上拉电阻,高电平由它们提供;
 *         开漏输出只能拉低,正好和"线与"总线的语义一致,也不会和拉伸时钟的从机对顶。
 */
void i2c_bus_config(const i2c_bus_t *bus)
{
    GPIO_InitTypeDef gpio_InitTypeDef = {0};

    RCC_AHB1PeriphClockCmd(bus->rcc_ahb1, ENABLE);

    // 两条线的 ODR 复位值是 0，若先配成输出再置 1，SDA=0、SCL=0 会把两条线都拉低，
    // 是从机无法识别的异常状态，要先置 1（释放总线）再切换成输出。
    i2c_sda_write(bus, 1);
    i2c_scl_write(bus, 1);

    gpio_InitTypeDef.GPIO_Mode  = GPIO_Mode_OUT;
    gpio_InitTypeDef.GPIO_Speed = GPIO_High_Speed;
    gpio_InitTypeDef.GPIO_PuPd  = GPIO_PuPd_NOPULL;

    // SCL 用开漏：板上 SCL 有外部上拉电阻，高电平靠它提供。
    // 实测推挽也能跑（总线上只有主机驱动 SCL），但开漏更稳妥：
    // 万一从机拉伸时钟，推挽会和从机对顶，开漏不会。
    gpio_InitTypeDef.GPIO_OType = GPIO_OType_OD;
    gpio_InitTypeDef.GPIO_Pin   = bus->scl_pin;
    GPIO_Init(bus->scl_port, &gpio_InitTypeDef);

    // SDA 用开漏：从机要靠拉低 SDA 回应答，推挽会和从机对顶。
    gpio_InitTypeDef.GPIO_OType = GPIO_OType_OD;
    gpio_InitTypeDef.GPIO_Pin   = bus->sda_pin;
    GPIO_Init(bus->sda_port, &gpio_InitTypeDef);
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

// 🟡 L1 架构代码 —— 主机在第九个时钟自己拉/放 SDA 表态的协议封装,ACK(拉低)与 NACK(放开)写反,从机会把"继续发"听成"到此为止"
// 主机接收从机数据时，主机回应的让从机继续发的应答
void i2c_master_ack(const i2c_bus_t *bus, uint8_t ack)
{
    i2c_scl_write(bus, 0);
    if (ack)
    {
        i2c_sda_write(bus, 0); // ACK，主机告诉从机继续发
    }
    else
    {
        i2c_sda_write(bus, 1); // NACK，主机告诉从机停止发
    }
    Delay_Us(4);

    i2c_scl_write(bus, 1);
    Delay_Us(4); // 从机在上升沿采样，高电平保持一会儿

    i2c_scl_write(bus, 0); // 结束当前ACK/NACK周期： I2C协议规定，数据位（包括ACK位）在SCL高电平期间必须保持稳定。将SCL拉低标志着这个应答位的采样窗口正式关闭。
    Delay_Us(4); // 保证SCL低电平宽度： I2C标准对SCL低电平持续时间有最小值要求（Standard Mode ≥ 4.7μs，Fast Mode ≥ 1.3μs）。这个延时确保即使后续代码立即执行，也不会违反时序。
}

// 🟡 L1 架构代码 —— 读从机应答的采样窗口(先放开 SDA 再喂一个时钟),返回 1=ACK 与常见驱动库相反,极性理解反了上层每次读写都会判错
// 从机接收主机数据后，返回的收应答
uint8_t i2c_master_wait_ack(const i2c_bus_t *bus)
{
    uint8_t ack = 0;

    i2c_scl_write(bus, 0); // 在释放 SDA 之前，必须先拉低 SCL，确保处于数据准备阶段。
    i2c_sda_write(bus, 1); // sda拉高，数据线空闲状态，不影响引脚输入

    // 这就是软件模拟 I2C 的一个时钟周期。
    // I2C 的时钟全部由主机产生，从机自己没有时钟，必须靠主机在 SCL 上拉低→拉高来"喂"节拍：
    i2c_scl_write(bus, 0);
    Delay_Us(4);
    i2c_scl_write(bus, 1);

    //返回 1=ACK、0=NACK(调用方按这个极性判断),和很多驱动库(0=ACK)相反
    if (i2c_sda_read(bus))
    {
        ack = 0; // 从机没把sda拉低，从机无应答
    }
    else
    {
        ack = 1;
    }
    Delay_Us(4);

    i2c_scl_write(bus, 0);
    Delay_Us(4);
    return ack; // ack==1从机有应答，ack==0从机无应答
}

// 🟡 L1 架构代码 —— 起始条件=SDA 在 SCL 高电平期间的 1→0 跳变,所以先把两条线拉高再让 SDA 落下;顺序错了从机只会当成一个普通数据位
// 开始条件的定义要求 SDA 从 1→0 跳变
void i2c_master_start(const i2c_bus_t *bus)
{

    i2c_sda_write(bus, 1); // 确保sda拉高
    i2c_scl_write(bus, 1); // 拉高时钟线，让从机接收数据线的数据
    Delay_Us(4);
    i2c_sda_write(bus, 0); // sda拉低，完成1到0的跳变
    Delay_Us(4);
    i2c_scl_write(bus, 0); // 拉低时钟线，衔接下次传输的时钟线状态
}

// 🟡 L1 架构代码 —— 停止条件=SDA 在 SCL 高电平期间的 0→1 跳变;先强制 SDA=0 是防御性前提,保证不管上一帧停在哪都能送出 STOP 而不是又一个 START
/*
停止条件的定义要求 SDA 从 0→1 跳变，所以 SDA 必须先处于 0"。
代码中写 SDA=0 是为了确保这个前提条件成立,
不管之前 SDA 是什么状态，都强制拉到 0，然后再在 SCL=1 时释放为 1，产生停止条件
*/
void i2c_master_stop(const i2c_bus_t *bus)
{
    i2c_sda_write(bus, 0); // 确保 SDA = 0，来完成一个从0->1的跳变（可能已经是0了，但保险起见再写一次）
    i2c_scl_write(bus, 1);
    Delay_Us(4);
    i2c_sda_write(bus, 1); // ← SDA 拉高 → STOP 信号
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

// 🟡 L1 架构代码 —— 把一字节拆成 8 个 SCL 周期、MSB 先出(多主仲裁要靠高位先比),末尾自动读一次应答,是 I2C 字节级协议的封装
// 发送数据（data为发送的数据）
uint8_t i2c_master_write(const i2c_bus_t *bus, uint8_t data)
{
    uint8_t ack = 0;
    for (int i = 0; i < 8; i++) // 依次发8位
    {
        i2c_scl_write(bus, 0);
        if (data & (0x80 >> i)) // 从高位发送
        {
            i2c_sda_write(bus, 1);
        }
        else
        {
            i2c_sda_write(bus, 0);
        }
        Delay_Us(4);
        i2c_scl_write(bus, 1);
        Delay_Us(4); // 至此完整一周期
    }
    ack = i2c_master_wait_ack(bus); // 接收从机的收应答
    return ack;                     //
}

// 🟡 L1 架构代码 —— 主机在 SCL 低电平放线、高电平采样凑满 8 位,尾随 ACK/NACK 决定从机还发不发;采样相位或位序理解错,读回的就是移位后的垃圾
// 接收数据
uint8_t i2c_master_read(const i2c_bus_t *bus, uint8_t ack)
{
    uint8_t data = 0;
    i2c_sda_write(bus, 1); // sda拉高，数据线空闲不影响引脚输入（根据GPIO图）
    for (int i = 0; i < 8; i++)
    {
        data <<= 1;

        // 这就是软件模拟 I2C 的一个时钟周期。
        // I2C 的时钟全部由主机产生，从机自己没有时钟，必须靠主机在 SCL 上拉低→拉高来"喂"节拍：
        i2c_scl_write(bus, 0);
        Delay_Us(4);
        i2c_scl_write(bus, 1);

        if (i2c_sda_read(bus))
        {
            data |= 1;
        }
        Delay_Us(4);
    }
    i2c_master_ack(bus, ack); // 主机发一位应答，ACK告诉从机继续发，NACK(0)告诉从机停止发
    return data;

    // 主机在每一个时钟沿上都等从机放线,任何停顿(第一帧的 16ms 测量、流中间的湿度补偿停顿)都自然被等待消化,不再依赖运气。
}
