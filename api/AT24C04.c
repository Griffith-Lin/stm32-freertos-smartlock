#include "AT24C04.h"

//scl pa8    sda pc9

// AT24C04A:4Kbit = 512 字节 I2C EEPROM
//
// 和 AT24C02(256 字节)最大的区别:容量翻倍后需要 9 位地址,但发完器件地址之后
// 只能再发 8 位"字地址",差的那一位被塞进了器件地址里,叫 P0:
//
//     器件地址(8 位) = 1 0 1 0 A2 A1 P0 R/W
//                      固定    地址脚  ↑
//                                     └─ 第 9 位地址
//
//     P0 = 0 → 0xA0 写 / 0xA1 读 → 访问地址 0   ~ 255
//     P0 = 1 → 0xA2 写 / 0xA3 读 → 访问地址 256 ~ 511
//
// 所以地址参数必须是 uint16_t(8 位装不下 512 个地址),而且每次发器件地址前
// 都要先按地址算出 P0。另外页大小也从 AT24C02 的 8 字节变成了 16 字节。

#define AT24C04_PAGE_SIZE  16       // 页大小:AT24C02 是 8,AT24C04A 是 16

// 内部写周期 tWR:STOP 之后器件才真正把数据写进浮栅,这期间对任何命令都不响应。
// 数据手册不同版本标 5ms / 10ms 两种上限,这里取大的保险。
#define AT24C04_TWR_MS     10

// 由存储地址算出器件地址字节:把 addr 的第 9 位(bit8)挪到器件地址的 P0 位(bit1)
#define AT24C04_ADDR_W(addr)  ((uint8_t)(0xA0u | (((addr) >> 7) & 0x02u)))
#define AT24C04_ADDR_R(addr)  ((uint8_t)(0xA1u | (((addr) >> 7) & 0x02u)))

// 字地址只发低 8 位,高的一位已经由 P0 表达了
#define AT24C04_WORD(addr)    ((uint8_t)((addr) & 0xFFu))


/**
 * 🟡 L1 架构代码 —— 页写地址在页内会回卷:len 一超本页剩余空间,新数据就从页头绕回来盖掉刚写的内容;这里把职责切成"只写一页以内 + STOP 后等满 tWR",跨页怎么拆是上层的事
 * @brief  向 EEPROM 写一页以内的数据(本函数不负责跨页)
 * @param  addr 起始存储地址(0~511)
 * @param  arr  待写入的数据
 * @param  len  写入字节数,必须 <= 本页剩余空间,超了会在页内回卷覆盖前面的数据
 * @return 0=成功;1=器件地址无应答;2=字地址无应答;3=数据无应答
 * @note   写完要等 tWR 才能发下一条命令,期间器件对任何命令都不响应。
 *         在 FreeRTOS 任务里调用时要把这个等待换成 vTaskDelay(原因见文件末尾)。
 */
uint8_t AT24C04_page_write(uint16_t addr, uint8_t *arr, uint16_t len)
{
    i2c_master_start(&i2c_bus_at24c04); // 主机发送起始条件

    if (!i2c_master_write(&i2c_bus_at24c04,AT24C04_ADDR_W(addr))) // 器件写地址 + 读写位
    {
        i2c_master_stop(&i2c_bus_at24c04);
        return 1;
    }

    if (!i2c_master_write(&i2c_bus_at24c04,AT24C04_WORD(addr))) // 字地址(低 8 位)
    {
        i2c_master_stop(&i2c_bus_at24c04);
        return 2;
    }

    while (len--)
    {
        if (!i2c_master_write(&i2c_bus_at24c04,*arr++)) // 数据
        {
            i2c_master_stop(&i2c_bus_at24c04);
            return 3;
        }
    }

    i2c_master_stop(&i2c_bus_at24c04); // 停止条件:器件从这里开始内部写周期

    Delay_Ms(AT24C04_TWR_MS); // 等写周期结束

    return 0;
}


/**
 * 🟡 L1 架构代码 —— 随机读要先发一次"只送地址不送数据"的假写把器件内部地址指针挪到位,再用重复 START 转读;最后一个字节必须回 NACK,写反了器件以为还要继续、一直占着 SDA,后面 START/STOP 都做不干净
 * @brief  从指定地址连续读出若干字节
 * @param  addr 起始存储地址(0~511)
 * @param  buf  接收缓冲区
 * @param  len  读取字节数
 * @return 0=成功;1=器件地址无应答;2=字地址无应答
 * @note   分两段事务:先做一次"假写"(只发器件地址+字地址、不发数据),
 *         把器件内部的地址指针挪到 addr;然后重复 START 发读地址开始读。
 *         最后一个字节主机必须回 NACK,否则器件会以为还要继续,一直输出。
 */
uint8_t AT24C04_read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    uint16_t i;

    i2c_master_start(&i2c_bus_at24c04);

    if (!i2c_master_write(&i2c_bus_at24c04,AT24C04_ADDR_W(addr))) // 假写:器件写地址
    {
        i2c_master_stop(&i2c_bus_at24c04);
        return 1;
    }

    if (!i2c_master_write(&i2c_bus_at24c04,AT24C04_WORD(addr))) // 假写:字地址
    {
        i2c_master_stop(&i2c_bus_at24c04);
        return 2;
    }

    i2c_master_start(&i2c_bus_at24c04); // 重复 START(不是 STOP+START,中途不放开总线)

    if (!i2c_master_write(&i2c_bus_at24c04,AT24C04_ADDR_R(addr))) // 器件读地址
    {
        i2c_master_stop(&i2c_bus_at24c04);
        return 1;
    }

    for (i = 0; i < len; i++)
    {
        // ⚠️ i2c_master_read 的参数:1 = 主机回 ACK("继续发"),0 = 回 NACK("够了")
        //    所以最后一个字节要传 0,别写反
        buf[i] = i2c_master_read(&i2c_bus_at24c04,(i == (uint16_t)(len - 1)) ? 0 : 1);
    }

    i2c_master_stop(&i2c_bus_at24c04);

    return 0;
}


/**
 * 🟡 L1 架构代码 —— 首段要先算出"当前页还剩几字节"再逐段推进,长度切错就会触发页内回卷、静默盖掉旧数据;而且失败只 printf 不给返回码,调用方拿不到错误信号
 * @brief  写任意长度数据,自动按页边界拆成多次页写
 * @param  addr 起始存储地址(0~511)
 * @param  arr  待写入的数据
 * @param  len  写入字节数
 * @return 无
 * @note   页写的地址在页内是"回卷"的:写超一页不会进位到下一页,
 *         而是绕回本页开头把前面的数据覆盖掉。所以超过页边界的写入必须拆开。
 */
void AT24C04_crosspage_write(uint16_t addr, uint8_t *arr, uint16_t len)
{
    uint16_t len_temp;

    if (len == 0)
    {
        return;
    }

    // 第一段:填满当前页剩余的空间
    len_temp = (uint16_t)(AT24C04_PAGE_SIZE - (addr % AT24C04_PAGE_SIZE));
    if (len_temp >= len)
    {
        len_temp = len;
    }

    while (1)
    {
        uint8_t ret = AT24C04_page_write(addr, arr, len_temp);
        if (ret)
        {
            // 1/2/3 分别对应器件地址、字地址、数据阶段没收到应答
            printf("EEPROM 写入失败(步骤 %d,地址 %d)\r\n", ret, addr);
            break;
        }

        if (len_temp == len)
        {
            break; // 全部写完
        }

        len  -= len_temp;
        addr += len_temp;
        arr  += len_temp;

        // 后面每段都是一整页,最后一段取剩余
        len_temp = (len > AT24C04_PAGE_SIZE) ? AT24C04_PAGE_SIZE : len;
    }
}

// ── 关于在 FreeRTOS 任务里使用本驱动 ────────────────────────────────────────
// 上面用的是 Delay_Ms(),它是 NOP 忙等,会把 CPU 占死 tWR 那么久,同优先级和
// 低优先级的任务全部饿死。如果这些函数是在任务里调用的,把:
//     Delay_Ms(AT24C04_TWR_MS);
// 换成:
//     vTaskDelay(pdMS_TO_TICKS(AT24C04_TWR_MS));
// 更专业的做法是"应答查询":循环发 START + 器件地址,收到 ACK 就说明写完了,
// 这样既不用猜时间,也不会等过头。
