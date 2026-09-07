#include "spi.h"


/*---------spi通信（硬件实现）----------*/


void spi1_w25_Config(void)
{
    
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1,ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA,ENABLE);
    
    
    GPIO_InitTypeDef gpio_InitTypeDef={0};
    
    gpio_InitTypeDef.GPIO_Mode=GPIO_Mode_AF;
    gpio_InitTypeDef.GPIO_OType=GPIO_OType_PP;
    gpio_InitTypeDef.GPIO_Speed=GPIO_High_Speed;
    gpio_InitTypeDef.GPIO_PuPd=GPIO_PuPd_NOPULL;
    gpio_InitTypeDef.GPIO_Pin=GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7;
    
    GPIO_Init(GPIOA,&gpio_InitTypeDef);
    
    SPI_CS = 1;      //  片选输出必须初始化为高(未选中),否则复位后第一次事务无下降沿
    
    
    gpio_InitTypeDef.GPIO_Pin=GPIO_Pin_4;
    gpio_InitTypeDef.GPIO_Mode=GPIO_Mode_OUT;
    gpio_InitTypeDef.GPIO_Speed=GPIO_Low_Speed;
    
    GPIO_Init(GPIOA,&gpio_InitTypeDef);
    
    
    GPIO_PinAFConfig(GPIOA,GPIO_PinSource5,GPIO_AF_SPI1);
    GPIO_PinAFConfig(GPIOA,GPIO_PinSource6,GPIO_AF_SPI1);
    GPIO_PinAFConfig(GPIOA,GPIO_PinSource7,GPIO_AF_SPI1);    
    
    
    SPI_InitTypeDef spi_InitTypeDef={0};
    
    spi_InitTypeDef.SPI_CPOL=SPI_CPOL_Low;
    spi_InitTypeDef.SPI_CPHA=SPI_CPHA_1Edge;
    spi_InitTypeDef.SPI_DataSize=SPI_DataSize_8b;
    spi_InitTypeDef.SPI_BaudRatePrescaler=SPI_BaudRatePrescaler_2;
    spi_InitTypeDef.SPI_Mode=SPI_Mode_Master;
    spi_InitTypeDef.SPI_FirstBit=SPI_FirstBit_MSB;
    spi_InitTypeDef.SPI_NSS=SPI_NSS_Soft ;
    spi_InitTypeDef.SPI_Direction=SPI_Direction_2Lines_FullDuplex;
    
    SPI_Init(SPI1,&spi_InitTypeDef);
    
    SPI_Cmd(SPI1, ENABLE);

}




//SPI数据收发一体函数 先发后收
uint8_t SPI_Echo(uint8_t data)
{
	uint32_t timeout = 0xFFFF;//如果硬件出问题（MISO 短路、W25Qxx 损坏），两个 while 会永久死等。加超时
    while((SPI_GetFlagStatus(SPI1,SPI_FLAG_TXE)==RESET) && --timeout);//等待发送缓冲区为空
    SPI_SendData(SPI1,data);
    timeout = 0xFFFF;
    while((SPI_GetFlagStatus(SPI1,SPI_FLAG_RXNE)==RESET) && --timeout);//等待接收缓冲区收到,标志位的定义一定要点进去看，标志位的逻辑不一定一样，有的为空，有的为非空
    return SPI_ReceiveData(SPI1);
}





//pa4 sd_cs   pa5 spi1_sck   pa6 spi1_miso   pa7 spi1_mosi   
//sp0
void spi1_sd_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStructure = {0};
	SPI_InitTypeDef SPI_InitStruct		= {0};
	
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);	//开外设时钟
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1,  ENABLE);
	
	GPIO_PinAFConfig(GPIOA, GPIO_PinSource5, GPIO_AF_SPI1);
	GPIO_PinAFConfig(GPIOA, GPIO_PinSource6, GPIO_AF_SPI1);
	GPIO_PinAFConfig(GPIOA, GPIO_PinSource7, GPIO_AF_SPI1);
	
	GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7;
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF;		//复用模式
	GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;		//推挽输出
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;	//50MHz
	GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_NOPULL;	//浮空
	GPIO_Init(GPIOA, &GPIO_InitStructure);				//初始化
    
        
	SPI_InitStruct.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_2;	//二分频
	SPI_InitStruct.SPI_CPHA = SPI_CPHA_1Edge;						//第一个时钟边沿（高边沿）采样
//	SPI_InitStruct.SPI_CPOL = SPI_CPOL_High;						//时钟空闲为高
    SPI_InitStruct.SPI_CPOL = SPI_CPOL_Low;						//时钟空闲为低 
	SPI_InitStruct.SPI_CRCPolynomial = 0x7;							//CRC多项式
	SPI_InitStruct.SPI_DataSize  = SPI_DataSize_8b;					//八位数据帧格式
	SPI_InitStruct.SPI_Direction = SPI_Direction_2Lines_FullDuplex;	//双线双向全双工
	SPI_InitStruct.SPI_FirstBit  = SPI_FirstBit_MSB;				//高位先发
	SPI_InitStruct.SPI_Mode = SPI_Mode_Master;						//主模式
	SPI_InitStruct.SPI_NSS  = SPI_NSS_Soft;							//软件管理片选线
	SPI_Init(SPI1, &SPI_InitStruct);
	
	SPI_Cmd(SPI1, ENABLE);											//使能外设
}



/*-----------spi通信（软件模拟）------------*/
//pb14片选
//pa5
void spi_simulation_ini(void)
{
    //打开时钟（SPI1 PA PB）
    RCC->APB2ENR |=(1<<12);//使能spi1时钟
    RCC->AHB1ENR |=(1<<0);//使能gpioa时钟
    RCC->AHB1ENR |= (1<<1);   // GPIOB 时钟
    
    //pa5(sck，推挽输出) pa7(mosi，推挽输出)
    GPIOA->MODER &=~((0x03<<10) | (0x03<<14));//清零
    GPIOA->MODER |=((1<<10) | (1<<14));//功能模式为推挽输出
    GPIOA->OTYPER &=~(7<<5);//推挽 
    GPIOA->OSPEEDR &=~(0x3f<<10);
    GPIOA->OSPEEDR |=(0x2a<<10);//引脚速度 快速
    GPIOA->PUPDR &=~(0x3f<<10);//上拉下拉设置为无，和ODR、IDR有关   
    
    //pa6(miso，浮空输入)
    GPIOA->MODER &=~(0x03<<12);//清零，功能模式为输入
    GPIOA->OTYPER &=~(7<<5);//开漏
    GPIOA->OSPEEDR &=~(0x3f<<10);
    GPIOA->OSPEEDR |=(0x2a<<10);//引脚速度 快速
    GPIOA->PUPDR &=~(0x3f<<10);//上拉下拉设置为无，和ODR、IDR有关 
    
    //pb14(cs，通用输出）
    GPIOB->MODER &=~(3<<28);
    GPIOB->MODER |= (1<<28);//通用输出
    GPIOB->OTYPER &=~(1<<14);//推挽
    GPIOB->OSPEEDR &=~(3<<28);//低速，片选线不用高速
    GPIOB->PUPDR &=~(3<<28);//无上下拉
    SPI_CS = 1;   // 初始化完成后释放片选,输出电平为高，非选中状态    
}



/*
物理层:
同时收发（全双工）

代码层:
先发后收（因为要先触发时钟）

逻辑层:
发送和接收是同一个原子操作

关键点：不是"为什么要先发后收"，而是SPI机制决定了你必须通过发送来触发接收
*/
//从机W25Qxx需要SP0模式（99%都用sp0 sp3） 下降沿发送 上升沿接收。先发后收。
uint8_t SPI_simulation_Echo(uint8_t data)
{
	u8 i,rev_data=0;
	for(i=0;i<8;i++)
	{
		SPI_SCK=0;//下降沿   拉低SCK → 产生下降沿（数据输出边沿）  主机输出MOSI
        
		if(data & (0x80>>i))
		{
			SPI_MOSI=1;
		}
		else
		{
			SPI_MOSI=0;
		}
        
		SPI_SCK=1;//上升沿     拉高SCK → 产生上升沿（数据采样边沿）  主机采样MISO
		rev_data<<=1;         
		if(SPI_MISO)          
		{                     
			rev_data |=1;     
		}                     
        
	}
	return rev_data;    
}





//1.先发送高位 发送八位数据
void send_data(uint8_t data)
{
    uint8_t i;
    for(i=0;i<8;i++)
    {
        if(data & (0x80>>i))
        {
            //发送1;
        }
        else
        {
            //发送0;
        }
        //data<<=1;
    }
}







