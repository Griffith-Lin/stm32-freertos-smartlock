#include "usart1.h"


// 🟡 L1 架构代码 —— PA9/PA10 复用 AF7、波特率、收发模式、RXNE/IDLE 中断一次配齐,它是 printf 与上位机命令的唯一入口;这条中断的抢占优先级(3)高于 FreeRTOS 的 configMAX_SYSCALL,ISR 里不能碰 RTOS API
void usart1_Config(uint32_t brr)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA,ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1,ENABLE);
    
    GPIO_InitTypeDef gpio_InitTypeDef={0};
    
    gpio_InitTypeDef.GPIO_Mode=GPIO_Mode_AF;
    gpio_InitTypeDef.GPIO_OType=GPIO_OType_PP;
    gpio_InitTypeDef.GPIO_Speed=GPIO_Low_Speed;
    gpio_InitTypeDef.GPIO_PuPd=GPIO_PuPd_NOPULL;
    gpio_InitTypeDef.GPIO_Pin=GPIO_Pin_9 | GPIO_Pin_10;
    

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_USART1);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_USART1);
    GPIO_Init(GPIOA,&gpio_InitTypeDef);
    
    
    USART_InitTypeDef usart_InitTypeDef;
    
    usart_InitTypeDef.USART_BaudRate = brr;            // 库函数内部自动计算 div_m 和 div_f
    usart_InitTypeDef.USART_WordLength = USART_WordLength_8b; // CR1 &= ~(1<<12) -> 8位
    usart_InitTypeDef.USART_StopBits = USART_StopBits_1;      // CR2 &= ~(3<<12) -> 1位停止位
    usart_InitTypeDef.USART_Parity = USART_Parity_No;         // CR1 &= ~(1<<10) -> 无校验
    usart_InitTypeDef.USART_HardwareFlowControl = USART_HardwareFlowControl_None; // 无流控
    usart_InitTypeDef.USART_Mode = USART_Mode_Rx | USART_Mode_Tx; // CR1 |= (3<<2) -> 收发使能

    NVIC_InitTypeDef NVIC_InitStructure;
    NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 3; // 抢占优先级
    
    
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    
    NVIC_Init(&NVIC_InitStructure);

    USART_ITConfig(USART1,USART_IT_RXNE,ENABLE);//接收中断使能
    USART_ITConfig(USART1,USART_IT_IDLE,ENABLE);//空闲中断使能

    // 初始化 USART1
    USART_Init(USART1, &usart_InitTypeDef);
    
    //使能
    USART_Cmd(USART1, ENABLE);
    
}


// 🟢 L2 工具代码 —— 阻塞等 TXE 再写 DR,契约就是进去一个字节、返回时它已进入发送寄存器;printf 重定向最终都落到这里
void usart1_send_byte(uint8_t byte)
{
    while(!USART_GetFlagStatus(USART1,USART_FLAG_TXE));//等待接收标志位

    USART_SendData(USART1,byte);
}

// 🟢 L2 工具代码 —— 阻塞等 RXNE 再读 DR;但它不能和 USART1 中断接收混用,ISR 会先把 RXNE 里的字节取走,这里会永远等不到
uint8_t usart1_rev_byte(void)
{
    uint8_t byte;
    while(!USART_GetFlagStatus(USART1,USART_FLAG_RXNE));
    byte=USART_ReceiveData(USART1);
    return byte;
}


volatile uint8_t usart_flag=0;
volatile uint8_t str_buf[100];//用来接收串口数据

// 🔴 L0 生死线代码 —— RXNE 分支往 str_buf[i++] 堆字节却没有任何长度上限,主机在 IDLE 到来前连发超过 100 字节就越界写穿 RAM;IDLE 标志还只能靠读 SR+DR 清除
void USART1_IRQHandler(void) 
{
    static uint16_t i=0;
    if(USART_GetITStatus(USART1,USART_IT_RXNE))//接收中断
    {
        USART_ClearITPendingBit(USART1,USART_IT_RXNE);//清除标志位
        str_buf[i++]=USART_ReceiveData(USART1);
    }
    
    
    if(USART_GetITStatus(USART1,USART_IT_IDLE))//空闲中断
    {
        volatile uint32_t temp;
        temp=USART1->SR;
        temp=USART1->DR;
        (void)temp;
        
         str_buf[i]='\0';
        i=0;
        usart_flag=1;
    }
}





// 🟢 L2 工具代码 —— 以换行符结尾的裸接收循环,契约里没有长度参数:buffer 够不够只有调用者知道,超长行直接溢写
//串口从上位机接收不规范字符串(....\r\n)
//调用者传的 buffer 可能是 uint8_t buff[50]，但函数不知道，发 200 字节就溢出了,最好再接收一个长度max_len
void usart1_rev_string(uint8_t * buffer)
{
    while(1)
    {
        *buffer = usart1_rev_byte();
        
        if(*buffer == '\n')
        {
            break;
        }
        
        buffer++;
    }
    
    *(buffer+1)='\0';
}

// 🟢 L2 工具代码 —— 以结尾零字节为界逐字节踢给 usart1_send_byte,最后再补发一个零字节;阻塞时长取决于波特率和对端,发对了就行
//串口1发送字符串
void usart1_send_string(volatile uint8_t *str)//刚刚没有写volatile修饰 uint8_t *str，编译器警告了。传参时 volatile uint8_t * → uint8_t *，编译器认为你丢掉了 volatile 修饰，这是在提醒你："这个变量我本来每次都要从内存读的，传进去之后函数可能把它优化成寄存器缓存，你不怕吗？"
{
    while(*str!='\0')
	{
		usart1_send_byte(*str);
		str++;
	}
    usart1_send_byte('\0');//再发送一个'\0'规范发送出的字符串
}


// 🟢 L2 工具代码 —— 与 usart1_rev_string 同一模式,终止符换成结尾零字节;同样没有长度参数,也收不了含零字节的数据
//串口1接收规范字符串(....\0)
void usart1_rev_string_norm(uint8_t * buffer)
{
    while(1)
    {
        *buffer = usart1_rev_byte();
        
        if(*buffer == '\0')
        {
            break;
        }
        
        buffer++;
    }
    
    
}

// 🟢 L2 工具代码 —— 目前是两个空 if 的测试桩,只做字符串比较分发,命中后什么都不做;契约是 buff 必须以零字节结尾
//验证接收的字符串
void rev_process(const volatile char* buff)
{
    if(strcmp((const char *)buff,"led1_on")== 0)
    {
        
    }
    if(strcmp((const char *)buff,"led1_off")== 0)
    {
        
    }
}





