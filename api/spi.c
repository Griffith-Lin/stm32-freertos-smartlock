#include "spi.h"


/*---------spi通信（硬件实现）----------*/


// 🟡 L1 架构代码 —— 选 Mode0(CPOL=0/CPHA=1Edge)+2 分频去配 W25Q,PA5/6/7 复用成 SPI1、CS 切成输出前先写 ODR=1;这些是硬件"设计选择",选错表现为 W25Q 不响应而不是烧硬件
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




// 🔴 L0 生死线代码 —— 直接轮询 SPI1 的 TXE/RXNE 状态位、收发共用同一个 DR;标志语义看错就静默错位(超时到期后照样把数据写进去),W25Q 的每个字节都不可信
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





// 🟡 L1 架构代码 —— SPI1 主模式的另一套参数(CPOL=0/CPHA=1Edge、8 位、MSB 先发)复用给 SD 卡,分频/复用脚是配置选择;错的表现是卡不响应,要把卡侧模式要求搞清楚
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
// 🔴 L0 生死线代码 —— 用魔法掩码直接改 GPIOA/GPIOB 的 MODER/OTYPER/OSPEEDR/PUPDR,没有标准库兜底,掩码哪一位算错就把整组相邻引脚一起改掉
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



// 🔴 L0 生死线代码 —— 用位带宏手翻 SCK/MOSI 模拟 SPI,在上升沿采 MISO 拼字节;位序(0x80>>i)或边沿搞反一位,整条数据流就永久错位且没有超时兜底
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





// 🟢 L2 工具代码 —— 循环体被整段注释掉,现在调用它等于什么都没发生,只是个未完成的接口占位;黑盒验证也只能确认"没有副作用"
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




/*----------- spi3:接 RC522 刷卡模块(hardware) ------------*/
// PC10 spi3_sck   PC11 spi3_miso   PC12 spi3_mosi   (都是 AF6)
//
// 为什么 RC522 走硬件 SPI3,而 W25Q 还留着软件模拟那一份:
//   RC522 的寄存器访问是"每次就 2 个字节、还要频繁开关片选"的短事务,
//   软件模拟光 GPIO 翻转的固定开销就比数据本身还大。而 RC522 用的时钟
//   模式(CPOL=0/CPHA=1Edge)和 W25Q 完全一样,硬件外设拿来就能用。
//
// 分频为什么是 8:
//   SPI3 挂在 APB1 上(系统 84MHz,AHPB1 二分频 = 42MHz),8 分频 = 5.25MHz。
//   RC522 手册给的 SPI 时钟上限是 10MHz,取 5.25MHz 留了一倍余量 ——
//   刷卡对速度没有任何要求,时序稳比跑得快重要。
//
// ⚠ 片选(CS)和复位(RST)**不在这里配**:
//   它俩是 RC522 这个器件自己的脚(PA15 / PD2),不是 SPI3 总线的一部分,
//   所以引脚宏放在 rfid.h 里 —— 和 SPI1 那边 "总线归 spi.h、器件归 spi.h
//   的 SPI_CS" 是同一个套路,只是这次器件头文件不是 spi.h 而已。
//
// 🟡 L1 架构代码 —— 复用脚/分频/时钟极性都是"硬件设计选择",任一项配错的表现
// 都是"卡完全刷不到"而不是烧东西,但对面的 RC522 不会给你任何报错
void spi3_Config(void)
{
    GPIO_InitTypeDef gpio_InitTypeDef = {0};
    SPI_InitTypeDef  spi_InitTypeDef  = {0};

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI3, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);

    // PC10/PC11/PC12 → 复用推挽输出,无上下拉
    gpio_InitTypeDef.GPIO_Pin   = GPIO_Pin_10 | GPIO_Pin_11 | GPIO_Pin_12;
    gpio_InitTypeDef.GPIO_Mode  = GPIO_Mode_AF;
    gpio_InitTypeDef.GPIO_OType = GPIO_OType_PP;
    gpio_InitTypeDef.GPIO_Speed = GPIO_High_Speed;
    gpio_InitTypeDef.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    GPIO_Init(GPIOC, &gpio_InitTypeDef);

    GPIO_PinAFConfig(GPIOC, GPIO_PinSource10, GPIO_AF_SPI3);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource11, GPIO_AF_SPI3);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource12, GPIO_AF_SPI3);

    spi_InitTypeDef.SPI_Direction         = SPI_Direction_2Lines_FullDuplex;
    spi_InitTypeDef.SPI_Mode              = SPI_Mode_Master;
    spi_InitTypeDef.SPI_DataSize          = SPI_DataSize_8b;
    spi_InitTypeDef.SPI_CPOL              = SPI_CPOL_Low;    // RC522 = SPI 模式 0
    spi_InitTypeDef.SPI_CPHA              = SPI_CPHA_1Edge;  // 第一个边沿采样
    spi_InitTypeDef.SPI_NSS               = SPI_NSS_Soft;    // 片选自己用 GPIO 拉
    spi_InitTypeDef.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8;  // 42MHz/8 = 5.25MHz
    spi_InitTypeDef.SPI_FirstBit          = SPI_FirstBit_MSB;         // RC522 高位先发

    SPI_Init(SPI3, &spi_InitTypeDef);

    // ⚠ 这一行不能省,省了 SPI3 会**整个不工作**,而且一点报错都没有。
    //   原因:ST 的 F4 版 SPI_Init() 只是把 SPI_NSS_Soft(=0x0200,也就是 SSM 位)
    //   或进 CR1,它**从不碰 SSI 位**(那个函数里"Set SSM, SSI and MSTR bits"
    //   的注释是从 F1 版抄过来的,而 F1 的代码里确实有 if 分支去设 SSI,
    //   F4 这版被简化成了单纯或运算,注释没跟着改)。
    //   SSM=1 表示"内部 NSS 由 SSI 位决定",可 SSI=0 表示内部 NSS 被拉**低**
    //   —— 主模式下这会让 SPI 判定为 MODF(模式错误),硬件自动把 MSTR 和 SPE
    //   清零,外设直接停摆。现象是:片选在拉、数据线一动不动、RC522 永远寻不到卡。
    //   所以必须显式把 SSI 置 1(内部 NSS 拉高 = "没有别的 SPI 主机在抢总线")。
    SPI_NSSInternalSoftwareConfig(SPI3, SPI_NSSInternalSoft_Set);

    SPI_Cmd(SPI3, ENABLE);
}


// 🔴 L0 生死线代码 —— 和 SPI_Echo 是同一类:直接轮询 SPI3 的 TXE/RXNE 状态位、
// 收发共用同一个 DR。标志语义看错就会静默错位(超时到期后照样把数据写进去),
// 而 RC522 的寄存器读回来的是地址+数据两个字节,错位一个字节就是整包寄存器
// 全错,表现为"找不到卡"却完全没有报错。
uint8_t SPI3_Echo(uint8_t data)
{
    uint32_t timeout = 0xFFFF;  // 防死等:MISO 短路/RC522 没焊好时不能卡在这儿

    while ((SPI_GetFlagStatus(SPI3, SPI_FLAG_TXE) == RESET) && --timeout)
        ;
    SPI_SendData(SPI3, data);

    timeout = 0xFFFF;
    while ((SPI_GetFlagStatus(SPI3, SPI_FLAG_RXNE) == RESET) && --timeout)
        ;

    return SPI_ReceiveData(SPI3);
}







