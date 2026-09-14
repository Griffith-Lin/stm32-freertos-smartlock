#include "CY9CM.h"
#include "stdio.h"

// CY8CMBR3116 电容触摸按键(4x3 密码键盘)控制器
//   地址 0x37(7位)  写字节 0x6E / 读字节 0x6F
//   RES  PA1,低电平有效脉冲会复位
//   SCL  PB6    SDA  PB7
//
// 通信上有 4 个和普通 I2C 器件不一样的地方(参考已验证可用的工程):
//   1. 芯片在低功耗(Deep Sleep / Look-for-Touch)状态下会对地址回 NACK,
//      它被地址匹配唤醒后要过一会儿才进入 Active —— 期间一直 NACK。
//      手册要求主机反复重发(repeated START + 地址)直到收到 ACK。
//   2. 写寄存器时各字节之间不能太快,每个字节后都要留间隔。
//   3. 读寄存器分两段独立事务:先写寄存器地址并 STOP,再重新 START 发读地址。
//   4. 出厂芯片要先用 CY9CM_RegConfig() 写 128 字节配置才能正常工作。

#define CY9CM_BYTE_GAP_US   500     // 字节之间的间隔(参考工程取 delay_us(500))
#define CY9CM_ADDR_RETRY    2000    // 地址相位重试上限,防止芯片异常时死等


/**
 * @brief  CY8CMBR3116 的 RES 复位脚(PA1)初始化
 * @note   RES 低电平有效。GPIO 的 ODR 复位值是 0,所以必须先置 1 再配成输出,
 *         否则一配置成输出就把芯片摁在复位态,后面 I2C 一个字节都不会应答。
 */
void CY9CM_Config(void)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);

    GPIO_SetBits(GPIOA, GPIO_Pin_1);        // 先拉高(不复位),再切输出

    GPIO_InitTypeDef gpio_InitTypeDef = {0};
    gpio_InitTypeDef.GPIO_Mode  = GPIO_Mode_OUT;
    gpio_InitTypeDef.GPIO_OType = GPIO_OType_PP;
    gpio_InitTypeDef.GPIO_Speed = GPIO_High_Speed;
    gpio_InitTypeDef.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    gpio_InitTypeDef.GPIO_Pin   = GPIO_Pin_1;
    GPIO_Init(GPIOA, &gpio_InitTypeDef);
}


/**
 * @brief  发送从机地址字节,被 NACK 就反复重发,直到收到 ACK
 * @param  addr_byte  已含读写位的地址字节(0x6E 写 / 0x6F 读)
 * @return 1=收到应答   0=重试到上限仍未应答
 * @note   芯片低功耗时按设计就会 NACK 地址,这不是故障,重发即可。
 *         重试动作是"重新 START + 重发地址",不是重发寄存器号。
 */
static uint8_t cy9cm_wait_addr(uint8_t addr_byte)
{
    uint16_t i;

    for (i = 0; i < CY9CM_ADDR_RETRY; i++)
    {
        if (i) i2c_master_start();                      // 重试 = 重新发 START
        if (i2c_master_write(addr_byte)) return 1;      // 收到 ACK
    }
    return 0;
}


/**
 * @brief  向指定寄存器连续写入若干字节
 * @param  reg   起始寄存器地址(每写一个字节,芯片内部地址自动加一)
 * @param  len   写入字节数
 * @param  data  数据缓冲区
 * @return 0=成功;1=寄存器地址无应答;2=数据无应答;3=地址相位无应答
 */
uint8_t CY9CM_WriteData(uint8_t reg, uint8_t len, const uint8_t *data)
{
    i2c_master_start();

    if (!cy9cm_wait_addr(CY9CM_ADDR_W)) { i2c_master_stop(); return 3; }

    Delay_Us(CY9CM_BYTE_GAP_US);

    if (!i2c_master_write(reg)) { i2c_master_stop(); return 1; }

    Delay_Us(CY9CM_BYTE_GAP_US);

    while (len--)
    {
        if (!i2c_master_write(*data++)) { i2c_master_stop(); return 2; }
        Delay_Us(CY9CM_BYTE_GAP_US);
    }

    i2c_master_stop();
    return 0;
}


/**
 * @brief  从指定寄存器连续读出若干字节
 * @param  reg   起始寄存器地址(每读一个字节,芯片内部地址自动加一)
 * @param  len   读取字节数
 * @param  data  接收缓冲区
 * @return 0=成功;1=寄存器地址无应答;3=地址相位无应答
 * @note   分两段事务:先写寄存器指针并 STOP,再重新 START 发读地址。
 *         最后一个字节主机要回 NACK,告诉从机别再发了。
 */
uint8_t CY9CM_ReadData(uint8_t reg, uint8_t len, uint8_t *data)
{
    uint8_t i;

    i2c_master_start();
    if (!cy9cm_wait_addr(CY9CM_ADDR_W)) { i2c_master_stop(); return 3; }

    Delay_Us(CY9CM_BYTE_GAP_US);

    if (!i2c_master_write(reg)) { i2c_master_stop(); return 1; }

    Delay_Us(CY9CM_BYTE_GAP_US);

    i2c_master_stop();                                  // 第一段事务到此结束

    i2c_master_start();                                 // 第二段:读

    if (!cy9cm_wait_addr(CY9CM_ADDR_R)) { i2c_master_stop(); return 3; }

    for (i = 0; i < len; i++)
    {
        // 最后一个字节回 NACK,其余回 ACK
        data[i] = i2c_master_read((i == (uint8_t)(len - 1)) ? 0 : 1);
    }

    i2c_master_stop();
    return 0;
}


/**
 * @brief  芯片的 128 字节寄存器配置表(配置区 0x00~0x7F)
 * @note   这张表是从已验证可用的参考工程原样搬过来的,对应 4x3 密码键盘。
 *         芯片配置是和板子绑定的(传感器数量、CMOD 电容、灵敏度都不同),
 *         换板子要用 EZ-Click 工具重新导出这张表。
 */
static const uint8_t CY9CM_RegPara[128] = {
    0xFFu, 0x0Fu, 0xFBu, 0x0Fu, 0x00u, 0x00u, 0x00u, 0x00u,
    0xAAu, 0xAAu, 0xA9u, 0x00u, 0x80u, 0x80u, 0x80u, 0x80u,
    0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x80u,
    0x7Fu, 0x7Fu, 0x7Fu, 0x7Fu, 0x08u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x80u,
    0x05u, 0x00u, 0x00u, 0x02u, 0x00u, 0x02u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x1Eu, 0x1Eu, 0x00u,
    0x00u, 0x1Eu, 0x1Eu, 0x00u, 0x00u, 0x00u, 0x01u, 0x01u,
    0x00u, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0x00u, 0x00u, 0x00u, 0x20u, 0x03u, 0x01u, 0x63u,
    0x00u, 0x37u, 0x03u, 0x00u, 0x00u, 0x0Au, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xA1u, 0xD5u
};


/**
 * @brief  配置芯片寄存器(可选调用)
 * @param  set_mode  0=芯片已经配置过就跳过(推荐)   1=强制重新配置
 * @note   流程:读 0x8A 判断是否出厂状态 → 从 0x00 写 128 字节配置 →
 *         写 CTRL_CMD(0x86)=2(校验并保存到非易失存储器)→ 等 300ms →
 *         读 0x89 查执行结果 → 写 0x86=0xFF 复位让新配置生效。
 */
void CY9CM_RegConfig(uint8_t set_mode)
{
    uint8_t sys_status = 0;
    uint8_t error_code = 0;
    uint8_t cmd        = 0;
    uint8_t ret        = 0;
    uint8_t i;

    // 0x8A = 1 表示芯片还是出厂配置,没被配置过
    if (CY9CM_ReadData(0x8A, 1, &sys_status) == 0)
    {
        printf("芯片%s\r\n", sys_status ? "为出厂配置" : "已配置过");
    }

    if ((sys_status == 0) && (set_mode == 0))
    {
        return;                     // 已经配置过,不用重配
    }

    // 从寄存器地址 0x00 开始写 128 字节配置
    for (i = 0; i < 10; i++)
    {
        ret = CY9CM_WriteData(0x00, 128, CY9CM_RegPara);
        if (ret == 0) break;
    }
    if (ret != 0)
    {
        printf("寄存器配置写入失败\r\n");
        return;
    }
    printf("寄存器配置写入成功\r\n");

    // CTRL_CMD(0x86) 写 2:校验并保存到非易失存储器
    cmd = 2;
    for (i = 0; i < 10; i++)
    {
        ret = CY9CM_WriteData(0x86, 1, &cmd);
        if (ret == 0) break;
    }
    if (ret != 0)
    {
        printf("保存命令写入失败\r\n");
        return;
    }
    printf("保存命令写入成功\r\n");

    Delay_Ms(300);                  // 等命令执行完

    // 命令执行结果在 0x89
    CY9CM_ReadData(0x89, 1, &error_code);
    switch (error_code)
    {
        case 0:   printf("命令执行成功\r\n");           break;
        case 253: printf("配置写入失败\r\n");           break;
        case 254: printf("CONFIG_CRC 校验不匹配\r\n");  break;
        case 255: printf("命令无效\r\n");               break;
        default:  printf("未知错误:%d\r\n", error_code); break;
    }

    // CTRL_CMD(0x86) 写 0xFF:复位,让新配置生效
    cmd = 0xFF;
    CY9CM_WriteData(0x86, 1, &cmd);
    printf("配置完成\r\n");
}


/**
 * @brief  CY8CMBR3116 完整初始化
 * @note   必须在 i2c_master_Config() 之后调用。
 *         先给一个 RES 复位脉冲让芯片从确定状态启动,再按需写配置。
 */
void CY9CM_Init(void)
{
    CY9CM_Config();

    // 复位脉冲:低 100ms → 高 100ms
    GPIO_ResetBits(GPIOA, GPIO_Pin_1);
    Delay_Ms(100);
    GPIO_SetBits(GPIOA, GPIO_Pin_1);
    Delay_Ms(100);                  // 等芯片启动(手册的 TI2CBOOT)

    CY9CM_RegConfig(0);             // 已配置过的芯片会自动跳过
}


/**
 * @brief  读一次按键状态
 * @return 被按下的键字符('0'~'9'、'*'、'#'),没有按键返回 CY9CM_KEY_NONE
 * @note   按键状态在 0xAA(低字节)和 0xAB(高字节)两个寄存器里,
 *         12 个按键用到了 bit11,所以必须一次读 2 字节。
 */
uint8_t CY9CM_ReadKey(void)
{
    uint8_t  buf[2] = {0};
    uint16_t key_value;

    if (CY9CM_ReadData(0xAA, 2, buf) != 0)
    {
        return CY9CM_KEY_NONE;      // 读失败,当作没按键
    }

    key_value = (uint16_t)((uint16_t)buf[1] << 8) | buf[0];

    switch (key_value)
    {
        case 0x0008: return '1';
        case 0x0010: return '2';
        case 0x0100: return '3';
        case 0x0004: return '4';
        case 0x0020: return '5';
        case 0x0200: return '6';
        case 0x0001: return '7';
        case 0x0080: return '8';
        case 0x0800: return '9';
        case 0x0040: return '0';
        case 0x0002: return '*';
        case 0x0400: return '#';
        default:     return CY9CM_KEY_NONE;
    }
}


/**
 * @brief  带边沿检测的按键扫描:只在"刚按下"的那一次返回键字符
 * @return 新按下的键字符,没有新按键返回 CY9CM_KEY_NONE
 * @note   按键一直按着时不会重复返回,松手后再次按下才会再返回一次。
 */
uint8_t CY9CM_KeyScan(void)
{
    static uint8_t key_flag = 0;
    uint8_t key = CY9CM_ReadKey();

    if (key && !key_flag)
    {
        key_flag = 1;
        printf("按键: %c\r\n", key);
        return key;
    }
    if (!key && key_flag)
    {
        key_flag = 0;
    }
    return CY9CM_KEY_NONE;
}


static void cy9cm_scan_task(void *pvParameters)
{
    (void)pvParameters;                 // 消除"参数未使用"告警

    for (;;)
    {
        CY9CM_KeyScan();
        vTaskDelay(pdMS_TO_TICKS(10));  // 必须用 vTaskDelay,不能用 Delay_Ms
    }
}

void cy9cm_tasks_init(void)
{
      xTaskCreate(cy9cm_scan_task, "cy9cm_scan", 256, NULL, 2, NULL);
}
