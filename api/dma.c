#include "dma.h"


/*
Function name:DMA_Font_Config
Description:DMA的初始化
param:None
retval:None
Remarks:
USART1_RX DMA2的数据流5的通道4
*/
Font font={0};
void DMA_Font_Config(void)
{
	DMA_InitTypeDef dma_InitTypeDef={0};
	NVIC_InitTypeDef nvic_InitTypeDef={0};
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA2,ENABLE);
	USART_DMACmd(USART1,USART_DMAReq_Rx,ENABLE);//USART的DMA接收使能
	
	DMA_Cmd(DMA2_Stream5,DISABLE);
	
	dma_InitTypeDef.DMA_BufferSize=BUFFSIZE;//数据项数目
	dma_InitTypeDef.DMA_Channel=DMA_Channel_4;//通道
	dma_InitTypeDef.DMA_DIR=DMA_DIR_PeripheralToMemory;//搬运方向 外设到存储器
	dma_InitTypeDef.DMA_FIFOMode=DMA_FIFOMode_Disable;//直接模式
//	dma_InitTypeDef.DMA_FIFOThreshold=;
	dma_InitTypeDef.DMA_MemoryBurst=DMA_MemoryBurst_Single;
	dma_InitTypeDef.DMA_MemoryDataSize=DMA_MemoryDataSize_Byte;//存储器数据宽度
	dma_InitTypeDef.DMA_MemoryInc=DMA_MemoryInc_Enable;//地址递增
	dma_InitTypeDef.DMA_Mode=DMA_Mode_Circular;//循环模式 
	dma_InitTypeDef.DMA_PeripheralBurst=DMA_PeripheralBurst_Single;
	dma_InitTypeDef.DMA_PeripheralDataSize=DMA_PeripheralDataSize_Byte;//外设数据宽度
	dma_InitTypeDef.DMA_PeripheralInc=DMA_PeripheralInc_Disable;//外设地址不递增
	dma_InitTypeDef.DMA_Priority=DMA_Priority_High;//优先级
    
    dma_InitTypeDef.DMA_Memory0BaseAddr=(uint32_t)font.buff[0];//存储器0地址（要存到哪个缓冲区中）
	dma_InitTypeDef.DMA_PeripheralBaseAddr=(uint32_t)&(USART1->DR);//外设地址
	
	DMA_Init(DMA2_Stream5,&dma_InitTypeDef);
	
	DMA_DoubleBufferModeConfig(DMA2_Stream5,(uint32_t)font.buff[1],DMA_Memory_0);
	DMA_DoubleBufferModeCmd(DMA2_Stream5,ENABLE);
	
	DMA_ITConfig(DMA2_Stream5,DMA_IT_TC,ENABLE);//传输完成中断 每传输数据项数目后触发一次

	nvic_InitTypeDef.NVIC_IRQChannel=DMA2_Stream5_IRQn;
	nvic_InitTypeDef.NVIC_IRQChannelCmd=ENABLE;
	nvic_InitTypeDef.NVIC_IRQChannelPreemptionPriority=1;//占先
	nvic_InitTypeDef.NVIC_IRQChannelSubPriority=1;//次级
	NVIC_Init(&nvic_InitTypeDef);

	DMA_Cmd(DMA2_Stream5,ENABLE);
}


void DMA2_Stream5_IRQHandler(void)
{
	DMA_ClearITPendingBit(DMA2_Stream5,DMA_IT_TCIF5);
	
	font.sta=DMA_IT;
	font.len=BUFFSIZE;
}

/*
Function name:Font_Load
Description:字库下载函数
param:None
retval:None
Remarks:
*/
void Font_Load(void)
{
	u8 i;
	u16 cnt=0;
	printf("正在擦除...\r\n");
	for(i=0;i<30;i++)
	{
		W25Qxx_Block_Erase(i*65536);
	}
	printf("擦除完成,请上传文件\r\n");
	while(1)
	{
		switch(font.sta)
		{
			case Free_IT:
				break;
			case DMA_IT:
				font.sta=Free_IT;
				W25Qxx_CrossPage(font.addr,font.buff[font.flag],font.len);
				font.flag=!font.flag;//调换缓冲区
				font.addr+=font.len;//更新写入的起始地址
				printf("cnt=%d\r\n",cnt);
				cnt++;
				break;
			case Usart_IT:
				W25Qxx_CrossPage(font.addr,font.buff[font.flag],font.len);
				printf("写入完成\r\n");
				return;
		}
	}
	
}

