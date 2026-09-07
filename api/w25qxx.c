#include "w25qxx.h"

/*


容量：64M-bit，即 8MB（8兆字节）


通信接口：标准 SPI 接口，并向下兼容。支持 Standard SPI、Dual SPI 和 Quad SPI（四线 SPI），后者可显著提高读取速度

存储结构（开发时必须牢记）：
页 (Page)：256 字节（基本编程/写入单位）。
扇区 (Sector)：4KB（4096 字节，16页），共 2048 个扇区
块 (Block)：64KB (65536个字节，16 个扇区），共 128 个块
*/

// 根据时序处理时序代码 进行通信


//pa4 cs   pa5 sclk  pa6 miso  pa7 mosi
/*
Function name:W25Qxx_Read_ID
Description:读取芯片ID
param:None
retval:None
Remarks:
*/
void W25Qxx_Read_ID(void)
{
    uint16_t id = 0;

    SPI_CS = 0; // 拉低片选，开始通信。每个从设备各有一根独立的 CS 线。这是 SPI 协议的约定俗成，几乎所有 SPI 芯片（包括 W25Q64）都设计成 低电平有效（Active Low）。
    /*
    默认低电平有效的原因主要有两个：
    抗干扰：空闲时保持高电平，毛刺不容易误触发；拉低才是 "认真的"
    上拉电阻：CS 引脚通常有内部上拉电阻，上电默认就是高电平（未选中状态），不会乱响应
    */
    SPI_Echo(0x90); // 发送指令代码 90h（读 ID 指令）

    SPI_Echo(0x00); // 发送 24 位地址的第 1 字节（A23-A16）= 00h
    SPI_Echo(0x00); // 第二字节
    SPI_Echo(0x00); // 第三字节

    id = SPI_Echo(0xff); // 读第一个返回字节
    /*
    SPI 是全双工通信 —— 发数据的同时也在收数据。DR寄存器只有一个，不能同时进行收发
    读取阶段主机不需要发有效数据，就发 0xff（全 1，相当于 "空字节"）作为占位，
    同时从从机接收数据。读取第 1 个返回字节 → 制造商 ID = EFh

    W25Qxx 的 MISO 引脚在空闲时是上拉的（内部或外部）。发送 0xFF（全1）时，MOSI 也是高电平，
    与 MISO 空闲状态一致，不会产生额外的电流毛刺。如果发 0x00，MOSI 拉低而 MISO 可能还浮空在高电平，
    瞬间形成短路电流（虽然很小，但在低功耗设计中要注意）。
    */

    id = id << 8 | SPI_Echo(0xff); // 读取第 2 个返回字节 → 设备 ID = 16h，拼到高 8 位后面
    SPI_CS = 1;                    // 拉高片选，结束通信
    printf("id=%#x\r\n", id);      // 0xef16,  #：替代形式标志（Alternative form）。它的作用是强制在输出结果前加上进制前缀。
                                   //%#x 对 0 有特殊处理 — 不加 0x 前缀。这是 C 标准规定：
}

void W25Qxx_simulation_Read_ID(void)
{
    uint16_t id = 0;
    SPI_CS = 0;

    SPI_simulation_Echo(0x90);

    SPI_simulation_Echo(0x00);
    SPI_simulation_Echo(0x00);
    SPI_simulation_Echo(0x00);

    id = SPI_simulation_Echo(0xff); // 发送从机规定的时序，来接收
    id = id << 8 | SPI_simulation_Echo(0xff);
    SPI_CS = 1;
    printf("id=%#x\r\n", id);
}

// 不能写入有两种可能：1、上锁 2、已被写入，未复原成0xff
// 写使能
void W25Qxx_write_enable(void)
{

    SPI_CS = 0;

    SPI_Echo(0x06);

    SPI_CS = 1;
}

// 写禁能
void W25Qxx_write_disable(void)
{

    SPI_CS = 0;

    SPI_Echo(0x04);

    SPI_CS = 1;
}

// 读状态寄存器1的BUSY位
uint8_t W25Qxx_Read_SR1_busy(void)
{
    uint8_t rec_data = 0;
    SPI_CS = 0;

    SPI_Echo(0x05);

    rec_data = SPI_Echo(0xff); // 收8位的二进制数（1字节）

    SPI_CS = 1;
    if (rec_data & (1 << 0))
    {
        return 1; // 忙碌
    }
    else
    {
        return 0;
    }
}

// 上锁flash
void W25Qxx_Lock(void)
{
    SPI_CS = 0;
    SPI_Echo(0x01);
    SPI_Echo(0xff);
    SPI_CS = 1;
}

// 初始化状态寄存器1 2 （可解锁flash）
void W25Qxx_write_SR1(void)
{

    SPI_CS = 0;

    SPI_Echo(0x01);
    SPI_Echo(0x00);
    SPI_Echo(0x00);

    SPI_CS = 1;
}

// 扇区擦除（最小擦除单位）
void W25Qxx_Sector_Erase(uint8_t sector)
{
    sector *= 4096;

    while (W25Qxx_Read_SR1_busy())
        ; // 阻塞等待BUSY位置0
    W25Qxx_write_enable();

    SPI_CS = 0;

    SPI_Echo(0x20);

    // 24位地址，决定从哪个地址开始往后擦除
    SPI_Echo(sector >> 16);
    SPI_Echo(sector >> 8);
    SPI_Echo(sector >> 0);

    SPI_CS = 1;
    while (W25Qxx_Read_SR1_busy())
        ;
    W25Qxx_write_disable();
}

// 块擦除
void W25Qxx_Block_Erase(u32 addr)
{
    while (W25Qxx_Read_SR1_busy())
        ; // 等待空闲
    W25Qxx_write_enable();

    SPI_CS = 0;
    SPI_Echo(0xD8);
    SPI_Echo(addr >> 16);
    SPI_Echo(addr >> 8);
    SPI_Echo(addr);
    SPI_CS = 1;
    while (W25Qxx_Read_SR1_busy())
        ; // 等待擦除完成
    W25Qxx_write_disable();
}

// 全片擦除
void W25Qxx_Chip_Erase(void)
{
    while (W25Qxx_Read_SR1_busy())
        ; // 等待空闲
    W25Qxx_write_enable();

    SPI_CS = 0;
    SPI_Echo(0xC7);
    SPI_CS = 1;
    while (W25Qxx_Read_SR1_busy())
        ; // 等待擦除完成
    W25Qxx_write_disable();
}

/*
从软件工程架构和工业级驱动设计的标准来看，这个函数的 API 设计存在严重的逻辑矛盾和架构缺陷。
如果将其放入你的智能家居项目中，会极大增加上层业务代码的复杂度和出错概率。

将参数拆分为 block, sector, page_addr 是早期单片机教学的典型做法，但在现代嵌入式工程中是反模式 (Anti-pattern)。
高耦合：上层业务代码（如保存 Wi-Fi 密码）被迫去了解 W25Q64 的物理结构（1个Block=64KB，1个Sector=4KB）。如果未来你更换了芯片（如 W25Q128 或 GD25Q64，它们的 Block/Sector 划分可能不同），所有调用这个函数的上层代码都必须重写。
计算易错：block * 65536 + sector * 4096 这种计算极易因 sector 是“全局扇区号”还是“块内相对扇区号”而产生地址重叠或越界。

SPI_Echo(addr>>16); 存在隐式类型转换风险。虽然 SPI_Echo 可能接受 uint8_t，但显式加上 & 0xFF 是更严谨的 C 语言习惯。

页编程
void W25Qxx_Page_Program(uint8_t block,uint8_t sector,uint8_t page_addr,uint8_t*arr_data)
{
    uint32_t addr=block*65536+sector*4096+page_addr;

    while(W25Qxx_Read_SR1_busy());
    W25Qxx_write_enable();

    SPI_CS=0;

    SPI_Echo(0x02);

    SPI_Echo(addr>>16);
    SPI_Echo(addr>>8);
    SPI_Echo(addr>>0);

    for(int i=0;i<256;i++)
    {
        SPI_Echo(arr_data[i]);
    }

    SPI_CS=1;

    W25Qxx_write_disable();
    while(W25Qxx_Read_SR1_busy());
}

*/

/**
 * @brief  W25Qxx 单页编程 (底层驱动)
 * @param  addr: 24位线性绝对地址 (必须是 256 的整数倍，即页对齐)
 * @param  data: 指向待写入数据的指针 (长度固定为 256 字节)
 * @retval 0: 成功, -1: 参数错误(未对齐)
 */
int8_t W25Qxx_Page_Program(uint32_t addr, const uint8_t *data, uint32_t len)
{
    // 有跨写函数就不用了
    //     /* 1. 边界防御：强制要求页对齐,确保地址在页的首位 */
    //     if (addr % 256 != 0) {
    //         return -1; // 调用者传入了错误的地址
    //     }

    /* 2. 等待芯片空闲 (关键前置条件) */
    while (W25Qxx_Read_SR1_busy())
        ;

    /* 3. 获取写入权限 */
    W25Qxx_write_enable();

    /* 4. 发送指令与数据 */
    SPI_CS = 0;
    SPI_Echo(0x02);                // Page Program
    SPI_Echo((addr >> 16) & 0xFF); // 显式掩码，确保只取低8位
    SPI_Echo((addr >> 8) & 0xFF);
    SPI_Echo(addr & 0xFF);

    for (int i = 0; i < len; i++)
    {
        SPI_Echo(data[i]);
    }
    SPI_CS = 1; // 触发内部编程

    /* 5. 等待写入完成并关闭写保护 */
    while (W25Qxx_Read_SR1_busy())
        ;
    W25Qxx_write_disable();

    return 0;
}

/*

1.你保留了 if (addr % 256 != 0) 的检查。这是对 Flash 物理特性的误解。
 Page Program (0x02)：必须页对齐，否则会回卷覆盖。
 Read Data (0x03)：完全支持从任意地址开始，连续读取任意长度，
地址会自动递增，跨越页边界和扇区边界均无问题。强制要求页对齐会极大限制该函数的可用性。

2.函数返回类型是 uint8_t（无符号 8 位整数，范围 0~255），但错误时返回了 -1。
在 C 语言中，-1 赋值给 uint8_t 会被隐式转换为 255 (0xFF)。
虽然上层可以通过 if (ret != 0) 捕获错误，但这违背了类型语义。规范的返回值应为 int8_t 或明确的枚举/宏定义。

3.读取操作 (0x03) 是同步的 SPI 数据传输，不会触发 Flash 内部的编程或擦除状态机，因此绝对不会改变 BUSY 位。
读操作前后的 while(W25Qxx_Read_SR1_busy()); 是多余的，白白浪费 CPU 周期。

@param  data: 指向待写入数组的指针 (长度固定为 256 字节)

uint8_t W25Qxx_Page_Read(uint32_t addr,uint8_t *data,uint32_t len)
{
//     1. 边界防御：强制要求页对齐,确保地址在页的首位
    if (addr % 256 != 0 || len>256) {
        return -1; // 调用者传入了错误的地址
    }

//     2. 等待芯片空闲 (关键前置条件)
    while (W25Qxx_Read_SR1_busy());


    SPI_CS = 0;
    SPI_Echo(0x03);
    SPI_Echo((addr >> 16) & 0xFF);         // 显式掩码，确保只取低8位
    SPI_Echo((addr >> 8) & 0xFF);
    SPI_Echo(addr & 0xFF);

    for (int i = 0; i < 256; i++) {
        data[i]=SPI_Echo(0xff);
    }

    SPI_CS = 1;


    while (W25Qxx_Read_SR1_busy());


    return 0;
}

*/

/**
 * @brief  W25Qxx 读取数据 (支持任意地址、任意长度连续读取)
 * @param  addr: 24位线性绝对起始地址 (无需页对齐)
 * @param  data: 指向接收数据缓冲区的指针 (输出)
 * @param  len: 期望读取的字节数
 * @retval 0: 成功, -1: 参数错误 (如 len 为 0 或 data 为空指针)
 */
int8_t W25Qxx_Read_Bytes(uint32_t addr, uint8_t *data, uint32_t len)
{
    /* 1. 边界防御：检查指针和长度有效性 */
    if (data == NULL || len == 0)
    {
        return -1;
    }

    /* 2. 注意：Read 操作不需要检查 Busy，也不需要页对齐 */

    /* 3. 启动 SPI 读取事务 */
    SPI_CS = 0;
    SPI_Echo(0x03); // Read Data 指令
    SPI_Echo((addr >> 16) & 0xFF);
    SPI_Echo((addr >> 8) & 0xFF);
    SPI_Echo(addr & 0xFF);

    /* 4. 严格按照传入的 len 进行读取，防止缓冲区溢出 */
    for (uint32_t i = 0; i < len; i++)
    {
        data[i] = SPI_Echo(0xFF); // 发送 dummy byte 以接收数据
    }

    SPI_CS = 1; // 结束事务

    return 0;
}

// 跨页写函数
void W25Qxx_CrossPage(uint32_t addr, const uint8_t *buff, uint32_t len)
{
    uint32_t len_temp = 0;
    len_temp = 256 - addr % 256;
    if (len_temp >= len) // 该页剩余空间足够
    {
        len_temp = len;
    }

    while (1)
    {
        W25Qxx_Page_Program(addr, buff, len_temp);
        if (len_temp == len)
        {
            break;
        }
        len -= len_temp;
        addr += len_temp;
        buff += len_temp;
        if (len > 256)
        {
            len_temp = 256;
        }
        else
        {
            len_temp = len;
        }
    }
}
