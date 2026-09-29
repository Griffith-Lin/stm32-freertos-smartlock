#include "MG200.h"
#include "tim.h" // GetTim10Tick():mg200_rec_timeout() 靠它计时

//   DETECT  PC0  手指触摸传感器时输出高电平 → 下拉输入(空闲为低)
//   TXD     PC7  模块 TXD 进 MCU,USART6_RX → 复用输入
//   RXD     PC6  MCU 发往模块 RXD,USART6_TX → 复用推挽输出
//   PWR     PC1  电源？->推挽上拉输出

/**
 * 🔴 L0 生死线代码 —— PC1 直接给模块上下电、PC6/PC7 就是 USART6 的物理引脚:引脚模式或电平配错的现象是"模块完全不应答",和协议写错长得一模一样,能排查到怀疑人生
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

    Delay_Ms(1000); // 等待指纹模块启动
}

// 🟢 L2 工具代码 —— 纯字节搬运(等 TXE → 写 DR),没有状态也没有协议;唯一要守的契约是必须等 TXE,图快改成盲写 DR 会覆盖还没发出去的字节
void usart6_send_byte(uint8_t byte)
{
    while (!USART_GetFlagStatus(USART6, USART_FLAG_TXE))
        ; // 等待接收标志位

    USART_SendData(USART6, byte);
}

// 🟡 L1 架构代码 —— while 死等 RXNE、一个超时都没有:模块没接/没供电/波特率不对它就永远不返回,所以只能用于裸机阶段;放进任务必须换 mg200_rec_timeout 那条路
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

// 🔴 L0 生死线代码 —— 中断服务函数:清标志和读 DR 的顺序、IDLE 标志的单独清扫写错会让中断反复触发或悄悄吞字节;而且它和轮询接收互斥,一旦使能,usart6_rev_byte() 就永远等不到数据
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

// 🟡 L1 架构代码 —— 帧结构(0x6C + 收发地址 + 命令 + 参数 + 预留 + 累加和)全在这一处落定:校验和少算一个字段模块就直接丢帧,而现象是"偶尔没反应",极难定位
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

/**
 * 🟡 L1 架构代码 —— 校验和公式和"模块回的帧收发地址与上位机发出时相反(0x62/0x63)"这两条协议约定都压在这一个 return 里,写反了每帧都判非法,可打印出来的原始帧看着又没错
 * @brief  判断一帧是否合法:帧头 + 收发地址 + 校验和
 * @param  rec 已填好各字段的帧
 * @return 1 = 合法
 * @note   校验和 = 发送地址~最后一个数据字节之和的低 8 位。
 *         模块回的帧里发送地址是 0x62、接收地址是 0x63(和上位机发出去时相反)。
 */
static uint8_t mg200_frame_valid(const MG_rec_data *rec)
{
    return (rec->beg == 0x6C &&
            rec->send_addr == 0x62 &&
            rec->rec_addr == 0x63 &&
            rec->crc == (uint8_t)(0x62 + 0x63 + rec->cmd + rec->result +
                                  rec->para + rec->reserve));
}

// 🟡 L1 架构代码 —— 找到帧头后按固定 8 字节盲读,且 while(1) 找帧头本身没有超时:它只属于裸机阶段,模块一不应答就死等,任务里必须换 mg200_rec_timeout()
MG_rec_data mg200_rec(void)
{
    MG_rec_data rec = {0};

    while (1)
    {
        rec.beg = usart6_rev_byte(); // 起始码
        if (rec.beg == 0x6c)
        {
            break;
        }
    }

    rec.send_addr = usart6_rev_byte(); // 发送地址

    rec.rec_addr = usart6_rev_byte(); // 接收地址

    rec.cmd = usart6_rev_byte(); // 应答码

    rec.result = usart6_rev_byte(); // 返回值

    rec.para = usart6_rev_byte(); // 参数

    rec.reserve = usart6_rev_byte(); // 预留

    rec.crc = usart6_rev_byte(); // 校验和

    rec.valid = mg200_frame_valid(&rec);

    return rec;
}

/**
 * 🟡 L1 架构代码 —— 整套超时机制的地基:(GetTim10Tick()-start) 这种无符号回绕减法不能换成比较绝对时刻,比较符或起点写错,任务就永久卡在这一帧上
 * @brief  等一个字节,超时就走人(不返回 0 的话任务会永远卡在这)
 * @param  out        收到的字节
 * @param  start      起始时刻,来自 GetTim10Tick()
 * @param  timeout_ms 允许多久
 * @return 1 = 收到了;0 = 到点还没来
 */
static uint8_t usart6_rev_byte_timeout(uint8_t *out, uint32_t start, uint32_t timeout_ms)
{
    while (!USART_GetFlagStatus(USART6, USART_FLAG_RXNE))
    {
        if ((GetTim10Tick() - start) > timeout_ms)
        {
            return 0;
        }
    }

    *out = USART_ReceiveData(USART6);
    return 1;
}

// 🟡 L1 架构代码 —— 先逐字节同步 0x6C 再收余下 7 字节,这是"上电毛刺让整帧永久右移一字节且不自愈"那个历史 bug 的修复点,退回定长盲读就会复发;8 个字节共用同一个总时限也是这里的约定
uint8_t mg200_rec_timeout(MG_rec_data *rec, uint32_t timeout_ms)
{
    uint32_t start = GetTim10Tick();
    MG_rec_data r = {0};
    uint8_t b;

    /* 1. 先同步帧头:模块上电毛刺会在 RX 里留杂字节,必须逐字节找到 0x6C
          才继续,否则整帧会永久右移一个字节且错位不自愈 */
    do
    {
        if (!usart6_rev_byte_timeout(&b, start, timeout_ms))
        {
            *rec = r;
            return 0;
        }
    } while (b != 0x6C);

    r.beg = b;

    /* 2. 后面固定还有 7 个字节,共用同一个总时限 */
    if (!usart6_rev_byte_timeout(&r.send_addr, start, timeout_ms) ||
        !usart6_rev_byte_timeout(&r.rec_addr, start, timeout_ms) ||
        !usart6_rev_byte_timeout(&r.cmd, start, timeout_ms) ||
        !usart6_rev_byte_timeout(&r.result, start, timeout_ms) ||
        !usart6_rev_byte_timeout(&r.para, start, timeout_ms) ||
        !usart6_rev_byte_timeout(&r.reserve, start, timeout_ms) ||
        !usart6_rev_byte_timeout(&r.crc, start, timeout_ms))
    {
        *rec = r;
        return 0;
    }

    r.valid = mg200_frame_valid(&r);
    *rec = r;
    return 1;
}

// 🟢 L2 工具代码 —— 只把 rec.valid 翻成 1/0 再 printf,判定逻辑在 mg200_frame_valid() 里;这里最坏也就是打印得不好读
uint8_t check_rec(MG_rec_data rec)
{
    if (rec.valid)
    {
        printf("MG200 ACK cmd=%02X result=%02X para=%02X\r\n",
               rec.cmd, rec.result, rec.para);
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

// 🟡 L1 架构代码 —— 采满 3 枚才发 0x7F 注册的录入流程(count/put_hint 两个标志的配合);它还不返回,main 里它后面的初始化代码全靠"采满 3 枚"才跑得到
void mg200_add_finger(void)
{
    uint8_t count = 0;
    uint8_t put_hint = 0;
    while (1)
    {
        if (put_hint == 0)
        {
            printf("请放下手指 \r\n");
            put_hint = 1;
        }

        if (GPIO_ReadInputDataBit(GPIOC, GPIO_Pin_0) == SET && count < 3)
        {
            mg200_send(0x51, count); // 提取特征码
            if (check_rec(mg200_rec()))
            {
                count++;
            }
            else
            {
                printf("重新来 \r\n");
            };

            printf("请松手 \r\n");
            put_hint = 0;
            while (GPIO_ReadInputDataBit(GPIOC, GPIO_Pin_0) == SET)
                ;
        }

        if (count == 3)
        {
            mg200_send(0x7f, 0); // 注册用户
            if (check_rec(mg200_rec()))
            {
                printf("添加成功 \r\n");
            }
            else
            {
                printf("添加失败 \r\n");
            };

            break;
        }
    }
}

// 🟢 L2 工具代码 —— 发 0x55 → 收一帧 → 打印 para,没有分支也没有状态,输入输出对了就行;要注意它走的是无超时的 mg200_rec(),模块不应答会卡在这(仅供裸机阶段)
void mg200_get_figer_num(void)
{
    MG_rec_data rec;

    mg200_send(0x55, 0);
    rec = mg200_rec();
    if (check_rec(rec))
    {
        printf("指纹数为%d \r\n", rec.para);
    }
    else
    {
        printf("失败 \r\n");
    }
}

// 🟢 L2 工具代码 —— 同样是"发命令 → 收一帧 → 打印",代码本体没有会写错的逻辑;它是清空整个指纹库的破坏性命令,风险在"谁调用它"而不在这几行
void mg200_erase_all(void)
{
    MG_rec_data rec;

    mg200_send(0x54, 0);
    rec = mg200_rec();
    if (check_rec(rec))
    {
        printf("删除成功 \r\n");
    }
    else
    {
        printf("失败 \r\n");
    }
}

// 🟡 L1 架构代码 —— 1:N 搜索的流程 + "result==0 算命中"的协议契约落在这一处(指纹业务 finger_match() 沿用同一判断),顺带还有等待手指松开的 GPIO 阻塞循环
void mg200_compare(void)
{
    MG_rec_data rec;
    uint8_t count = 0;
    uint8_t put_hint = 0;
    while (1)
    {
        if (put_hint == 0)
        {
            printf("请放下手指 \r\n");
            put_hint = 1;
        }

        if (GPIO_ReadInputDataBit(GPIOC, GPIO_Pin_0) == SET && count < 3)
        {
            mg200_send(0x51, count); // 提取特征码
            if (check_rec(mg200_rec()))
            {
                count++;
            }
            else
            {
                printf("重新来 \r\n");
            };

            printf("请松手 \r\n");
            put_hint = 0;
            while (GPIO_ReadInputDataBit(GPIOC, GPIO_Pin_0) == SET)
                ;
        }

        if (count == 1)
        {
            mg200_send(0x71, 0);
            rec = mg200_rec();
            if (check_rec(rec))
            {
                printf("进入比对 \r\n");
                if (rec.result == 0)
                {
                    printf("比对成功 \r\n");
                }
                else
                {
                    printf("比对失败 \r\n");
                }
            }
            else
            {
                printf("失败 \r\n");
            }

            break;
        }
    }
}