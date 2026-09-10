#include "MG200.h"

//   DETECT  PC0  手指触摸传感器时输出高电平 → 下拉输入(空闲为低)
//   TXD     PC7  模块 TXD 进 MCU,USART6_RX → 复用输入
//   RXD     PC6  MCU 发往模块 RXD,USART6_TX → 复用推挽输出
//   PWR     PC1  电源？->推挽上拉输出

/**
 * @brief MG200 指纹模块初始化:4 根脚 + USART6(8N1,无流控)
 * @param brr 与模块约定的波特率(115200)
 * @return 无
 * @note PC0(DETECT)以后做"手指触碰检测";PC1(SIG_OUT)是模块信号输出控制。
 *       当前协议层使用轮询接收，因此不使能 USART6 接收中断，
 *       避免中断处理函数先读走 RXNE 数据，导致 usart6_rev_byte() 一直等待。
 */
void mg200_Config(uint32_t brr)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART6, ENABLE);

    GPIO_InitTypeDef gpio_InitTypeDef = {0};

    // PC0 DETECT:输入 + 内部下拉(空闲为低,模块输出高 = 有手指)
    gpio_InitTypeDef.GPIO_Mode = GPIO_Mode_IN;
    gpio_InitTypeDef.GPIO_PuPd = GPIO_PuPd_DOWN;
    gpio_InitTypeDef.GPIO_Pin = GPIO_Pin_0;
    GPIO_Init(GPIOC, &gpio_InitTypeDef);

    // PC1 PWR:推挽输出，拉高
    gpio_InitTypeDef.GPIO_Mode = GPIO_Mode_OUT;
    gpio_InitTypeDef.GPIO_OType = GPIO_OType_PP;
    gpio_InitTypeDef.GPIO_Speed = GPIO_Low_Speed;
    gpio_InitTypeDef.GPIO_PuPd = GPIO_PuPd_UP;
    gpio_InitTypeDef.GPIO_Pin = GPIO_Pin_1;
    GPIO_SetBits(GPIOC, GPIO_Pin_1);
    GPIO_Init(GPIOC, &gpio_InitTypeDef);

    // PC6=USART6_TX / PC7=USART6_RX:AF8 复用(收发都配 AF 推挽,与 usart1.c 同风格)
    gpio_InitTypeDef.GPIO_Mode = GPIO_Mode_AF;
    gpio_InitTypeDef.GPIO_OType = GPIO_OType_PP;
    gpio_InitTypeDef.GPIO_Speed = GPIO_Low_Speed;
    gpio_InitTypeDef.GPIO_PuPd = GPIO_PuPd_NOPULL;
    gpio_InitTypeDef.GPIO_Pin = GPIO_Pin_6 | GPIO_Pin_7;
    GPIO_Init(GPIOC, &gpio_InitTypeDef);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource6, GPIO_AF_USART6);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource7, GPIO_AF_USART6);

    // USART6 帧格式:8 数据位 + 1 停止位 + 无校验 + 无流控,收发都开
    USART_InitTypeDef usart_InitTypeDef;

    usart_InitTypeDef.USART_BaudRate = brr;
    usart_InitTypeDef.USART_WordLength = USART_WordLength_8b;
    usart_InitTypeDef.USART_StopBits = USART_StopBits_1;
    usart_InitTypeDef.USART_Parity = USART_Parity_No;
    usart_InitTypeDef.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart_InitTypeDef.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;



    USART_Init(USART6, &usart_InitTypeDef);

    USART_Cmd(USART6, ENABLE);

    Delay_Ms(1000);//等待指纹模块启动
}

void usart6_send_byte(uint8_t byte)
{
    while (!USART_GetFlagStatus(USART6, USART_FLAG_TXE))
        ; // 等待接收标志位

    USART_SendData(USART6, byte);
}

uint8_t usart6_rev_byte(void)
{
    uint8_t byte;
    while (!USART_GetFlagStatus(USART6, USART_FLAG_RXNE))
        ;
    byte = USART_ReceiveData(USART6);
    return byte;
}

volatile uint8_t str6_buf[100] = {0}; // 用来接收串口数据
volatile uint8_t usart6_flag = 0;

void USART6_IRQHandler(void)
{
    static uint16_t i = 0;
    if (USART_GetITStatus(USART6, USART_IT_RXNE)) // 接收中断
    {
        USART_ClearITPendingBit(USART6, USART_IT_RXNE); // 清除标志位
        if (i < sizeof(str6_buf) - 1)
        {
            str6_buf[i++] = USART_ReceiveData(USART6);
        }
        else
        {
            (void)USART_ReceiveData(USART6); // 丢弃超出缓冲区的数据，避免越界
        }
    }

    if (USART_GetITStatus(USART6, USART_IT_IDLE)) // 空闲中断
    {
        volatile uint32_t temp;
        temp = USART6->SR;
        temp = USART6->DR;
        (void)temp;
        usart6_flag = 1;
        str6_buf[i] = '\0';
        i = 0;
    }
}

void mg200_send(uint8_t cmd, uint8_t para)
{
    uint8_t crc = 0;

    usart6_send_byte(0x6c); // 起始码

    usart6_send_byte(0x63); // 发送地址

    usart6_send_byte(0x62); // 接收地址

    usart6_send_byte(cmd); // 命令

    usart6_send_byte(para); // 参数

    usart6_send_byte(0); // 预留

    crc = 0x63 + 0x62 + cmd + para;
    usart6_send_byte(crc); // 校验和

}

MG_rec_data mg200_rec(void)
{
    MG_rec_data rec = {0};

    rec.beg = usart6_rev_byte(); // 起始码

    rec.send_addr = usart6_rev_byte(); // 发送地址

    rec.rec_addr = usart6_rev_byte(); // 接收地址

    rec.cmd = usart6_rev_byte(); // 应答码

    rec.result = usart6_rev_byte(); // 返回值

    rec.para = usart6_rev_byte(); // 参数

    rec.reserve = usart6_rev_byte(); // 预留

    rec.crc = usart6_rev_byte(); // 校验和

    rec.valid = (rec.beg == 0x6C &&
                 rec.send_addr == 0x62 &&
                 rec.rec_addr == 0x63 &&
                 rec.crc == (uint8_t)(0x62 + 0x63 + rec.cmd + rec.result +
                                      rec.para + rec.reserve));

    return rec;
}


uint8_t check_rec(MG_rec_data rec)
{
    if (rec.valid)
    {
        printf("MG200 ACK cmd=%02X result=%02X \r\n",
               rec.cmd, rec.result);
        return 1;
    }
    else
    {
        printf("MG200 invalid: %02X %02X %02X %02X %02X %02X %02X %02X calc=%02X\r\n",
               rec.beg, rec.send_addr, rec.rec_addr, rec.cmd, rec.result,
               rec.para, rec.reserve, rec.crc,
               (uint8_t)(rec.send_addr + rec.rec_addr + rec.cmd + rec.result +
                         rec.para + rec.reserve));
        return 0;
    }
}


void mg200_add_finger(void)
{
    uint8_t count=0;   
    uint8_t put_hint=0;
    while (1)
    {
        if(put_hint==0)
        {
            printf("请放下手指\r\n");
            put_hint=1;
        }
        

        if(GPIO_ReadInputDataBit(GPIOC,GPIO_Pin_0)==SET && count<3)
        {          
            mg200_send(0x51, count);//提取特征码
            if(check_rec(mg200_rec()))
            {
               count++;     
            }
            else
            {
                printf("重新来\r\n");
            }
            ;
            

            printf("请松手\r\n");
            put_hint=0;
            while(GPIO_ReadInputDataBit(GPIOC,GPIO_Pin_0)==SET);
        }

        if(count==3)
        {
            mg200_send(0x7f, 0);//注册用户
            if(check_rec(mg200_rec()))
            {
                printf("添加成功\r\n");
            }
            else
            {
                printf("添加失败\r\n");
            };

            break;
        }
        
    }
}
