#include "rfid.h"
#include "spi.h"     // spi3_Config() / SPI3_Echo()  —— 换掉了原厂的 Spi3_Init/SPI3TransferByte
#include "delay.h"   // Delay_Us()                 —— 换掉了原厂的 delay_us()

// ══════════════════════════════════════════════════════════════════════════
//  原厂 RC522 移植代码的适配说明
//
//  这个文件是 MF522/RC522 的官方参考代码,协议部分(PcdRequest / PcdAnticoll /
//  PcdComMF522 / CalulateCRC ...)一行没动 —— 那些是照着芯片手册写的寄存器时序,
//  自己重写只会引入新 bug。改的只有"和平台绑定的那一层":
//
//    原厂写法                        本工程写法              为什么
//    ────────────────────────────    ────────────────────    ──────────────────
//    delay_us(n)                     Delay_Us(n)             delay.c 里现成的
//    SPI3TransferByte(b)             SPI3_Echo(b)            spi.c 里 SPI1 就叫
//                                                            SPI_Echo,统一命名
//    Spi3_Init()                     spi3_Config()           同上,和 spi1_w25_
//                                                            Config() 对齐
//    RFID_CS_H/L, RFID_RST_H/L       RFID_CS_H/L, RFID_RST_H/L
//                                    引脚宏移进 rfid.h 并改对了拼写
//    RIFD_SCLK_*/RIFD_MOSI_*/RFID_MISO   已删除              原厂靠位带翻软件 SPI,
//                                                            本工程用硬件 SPI3
//                                                            (PC10/11/12 是 AF6)
//
//  ⚠ 有两点原厂代码看着像 bug、实际是**有意为之**,别顺手"修":
//    · 状态变量用 char 而不是 uint8_t。MI_NOTAGERR(0xcc)/MI_ERR(0xbb) 都超过
//      0x7F,而 armcc 的 char 默认是无符号的,存进去和 MI_OK(0x26) 比较都正常。
//      真要"修"成 signed char 反而会让 0xcc 变成负数,判断全乱。
//    · PcdComMF522 里 ul = 1000 那个循环看着像硬编码的忙等,其实是**兜底**:
//      正常情况下 RC522 内部定时器(~15ms,TModeReg/TReloadReg 配的)会先
//      把 ComIrqReg 的定时器中断位置起来,循环靠那个条件退出。
// ══════════════════════════════════════════════════════════════════════════


/*************************************
  * @brief  读RC522寄存器
  * @param  ucAddress，寄存器地址
  * @retval 寄存器的当前值
*************************************/
uint8_t ReadRawRC ( uint8_t ucAddress )
{
	uint8_t ucAddr, ucReturn;
	
	ucAddr = ( ( ucAddress << 1 ) & 0x7E ) | 0x80;	
	RFID_CS_L;
  
	SPI3_Echo(ucAddr);
	ucReturn = SPI3_Echo(0);
  
	RFID_CS_H;
	
	return ucReturn;	
}
 
/**************************************
  * @brief  写RC522寄存器
  * @param  ucAddress，寄存器地址
  * @param  ucValue，写入寄存器的值
  * @retval 无
  *************************************/
void WriteRawRC ( uint8_t ucAddress, uint8_t ucValue )
{  
	uint8_t ucAddr;
	
	ucAddr = ( ucAddress << 1 ) & 0x7E;	
	RFID_CS_L;
	
	SPI3_Echo(ucAddr);	
	SPI3_Echo(ucValue);
  
	RFID_CS_H;		
}
 

/**************************************
  * @brief  对RC522寄存器置位
  * @param  ucReg，寄存器地址
  * @param   ucMask，置位值
  * @retval 无
  *************************************/
void SetBitMask ( uint8_t ucReg, uint8_t ucMask )  
{
  uint8_t ucTemp;
 
  ucTemp = ReadRawRC ( ucReg );
  WriteRawRC ( ucReg, ucTemp | ucMask ); // set bit mask
}


/**************************************
  * @brief  对RC522寄存器清位
  * @param  ucReg，寄存器地址
  * @param  ucMask，清位值
  * @retval 无
  *************************************/
void ClearBitMask ( uint8_t ucReg, uint8_t ucMask )  
{
  uint8_t ucTemp;
 
  ucTemp = ReadRawRC ( ucReg );
  WriteRawRC ( ucReg, ucTemp & ( ~ ucMask) ); // clear bit mask
}
 
/**
  * @brief  开启天线 
  * @param  无
  * @retval 无
  */
void PcdAntennaOn ( void )
{
  uint8_t uc;
 
  uc = ReadRawRC ( TxControlReg );
  if ( ! ( uc & 0x03 ) )
   SetBitMask(TxControlReg, 0x03);		
}


/**************************************
  * @brief  关闭天线
  * @param  无
  * @retval 无
  *************************************/
void PcdAntennaOff ( void )
{
  ClearBitMask ( TxControlReg, 0x03 );	
}


/**************************************
  * @brief  复位RC522 
  * @param  无
  * @retval 无
*************************************/
void PcdReset ( void )
{
	RFID_RST_H;
	Delay_Us(1);
	
	RFID_RST_L;
	Delay_Us(1);
	
	RFID_RST_H;
	Delay_Us(1);
	
	WriteRawRC ( CommandReg, 0x0f );
	while( ReadRawRC ( CommandReg ) & 0x10 );
	Delay_Us(1);
	
	//定义发送和接收常用模式 和Mifare卡通讯，CRC初始值0x6363
  WriteRawRC( ModeReg, 0x3D );        
  WriteRawRC( TReloadRegL, 30 );      //16位定时器低位    
	WriteRawRC( TReloadRegH, 0 );			 //16位定时器高位
  WriteRawRC( TModeReg,0x8D);			 //定义内部定时器的设置
  WriteRawRC( TPrescalerReg, 0x3E );	 //设置定时器分频系数
	WriteRawRC( TxAutoReg, 0x40 );			 //调制发送信号为100%ASK	
	
}

void Reset_RC522(void)
{
	PcdReset();       //功    能：复位RC522
	PcdAntennaOff();  //关闭天线
	PcdAntennaOn();   //开启天线
}    

/**************************************
  * @brief  设置RC522的工作方式
  * @param  ucType，工作方式
  * @retval 无
*************************************/
void M500PcdConfigISOType ( uint8_t ucType )
{
	if ( ucType == 'A')                     //ISO14443_A
  {
		ClearBitMask ( Status2Reg, 0x08 );
    WriteRawRC ( ModeReg, 0x3D );         //3F
		WriteRawRC ( RxSelReg, 0x86 );        //84
		WriteRawRC( RFCfgReg, 0x7F );         //4F
		WriteRawRC( TReloadRegL, 30 );        
		WriteRawRC ( TReloadRegH, 0 );
		WriteRawRC ( TModeReg, 0x8D );
		WriteRawRC ( TPrescalerReg, 0x3E );
		
		Delay_Us(2 );
		PcdAntennaOn ();//开天线
		
   }	 
}

/**************************************
  * @brief  通过RC522和ISO14443卡通讯
  * @param  ucCommand，RC522命令字
  * @param  pInData，通过RC522发送到卡片的数据
  * @param  ucInLenByte，发送数据的字节长度
  * @param  pOutData，接收到的卡片返回数据
  * @param  pOutLenBit，返回数据的位长度
  * @retval 状态值= MI_OK，成功
************************************/
char PcdComMF522 ( uint8_t ucCommand,
                   uint8_t * pInData, 
                   uint8_t ucInLenByte, 
                   uint8_t * pOutData,
                   uint32_t * pOutLenBit )		
{
  char cStatus = MI_ERR;
  uint8_t ucIrqEn   = 0x00;
  uint8_t ucWaitFor = 0x00;
  uint8_t ucLastBits;
  uint8_t ucN;
  uint32_t ul;
 
  switch ( ucCommand )
  {
     case PCD_AUTHENT:		  //Mifare认证
        ucIrqEn   = 0x12;		//允许错误中断请求ErrIEn  允许空闲中断IdleIEn
        ucWaitFor = 0x10;		//认证寻卡等待时候 查询空闲中断标志位
        break;
     
     case PCD_TRANSCEIVE:		//接收发送 发送接收
        ucIrqEn   = 0x77;		//允许TxIEn RxIEn IdleIEn LoAlertIEn ErrIEn TimerIEn
        ucWaitFor = 0x30;		//寻卡等待时候 查询接收中断标志位与 空闲中断标志位
        break;
     
     default:
       break;     
  }
  //IRqInv置位管脚IRQ与Status1Reg的IRq位的值相反 
  WriteRawRC ( ComIEnReg, ucIrqEn | 0x80 );
  //Set1该位清零时，CommIRqReg的屏蔽位清零
  ClearBitMask ( ComIrqReg, 0x80 );	 
  //写空闲命令
  WriteRawRC ( CommandReg, PCD_IDLE );		 
  
  //置位FlushBuffer清除内部FIFO的读和写指针以及ErrReg的BufferOvfl标志位被清除
  SetBitMask ( FIFOLevelReg, 0x80 );			
 
  for ( ul = 0; ul < ucInLenByte; ul ++ )
    WriteRawRC ( FIFODataReg, pInData [ ul ] ); //写数据进FIFOdata
    
  WriteRawRC ( CommandReg, ucCommand );					//写命令
 
 
  if ( ucCommand == PCD_TRANSCEIVE )
    
    //StartSend置位启动数据发送 该位与收发命令使用时才有效
    SetBitMask(BitFramingReg,0x80);  				  
 
  ul = 1000;                             //根据时钟频率调整，操作M1卡最大等待时间25ms
 
  do 														         //认证 与寻卡等待时间	
  {
       ucN = ReadRawRC ( ComIrqReg );		 //查询事件中断
       ul --;
  } while ( ( ul != 0 ) && ( ! ( ucN & 0x01 ) ) && ( ! ( ucN & ucWaitFor ) ) );	//退出条件i=0,定时器中断，与写空闲命令
 
  ClearBitMask ( BitFramingReg, 0x80 );	 //清理允许StartSend位
 
  if ( ul != 0 )
  {
    //读错误标志寄存器BufferOfI CollErr ParityErr ProtocolErr
    if ( ! ( ReadRawRC ( ErrorReg ) & 0x1B ) )	
    {
      cStatus = MI_OK;
      
      if ( ucN & ucIrqEn & 0x01 )				//是否发生定时器中断
        cStatus = MI_NOTAGERR;   
        
      if ( ucCommand == PCD_TRANSCEIVE )
      {
        //读FIFO中保存的字节数
        ucN = ReadRawRC ( FIFOLevelReg );		          
        
        //最后接收到得字节的有效位数
        ucLastBits = ReadRawRC ( ControlReg ) & 0x07;	
        
        if ( ucLastBits )
          
          //N个字节数减去1（最后一个字节）+最后一位的位数 读取到的数据总位数
          * pOutLenBit = ( ucN - 1 ) * 8 + ucLastBits;   	
        else
          * pOutLenBit = ucN * 8;      //最后接收到的字节整个字节有效
        
        if ( ucN == 0 )		
          ucN = 1;    
        
        if ( ucN > MAXRLEN )
          ucN = MAXRLEN;   
        
        for ( ul = 0; ul < ucN; ul ++ )
          pOutData [ ul ] = ReadRawRC ( FIFODataReg );   
        
        }        
    }   
    else
      cStatus = MI_ERR;       
  }
 
  SetBitMask ( ControlReg, 0x80 );           // stop timer now
  WriteRawRC ( CommandReg, PCD_IDLE ); 
   
  return cStatus;
}



/**************************************************************************
  * @brief 寻卡
  * @param  ucReq_code，寻卡方式 = 0x52，寻感应区内所有符合14443A标准的卡；
            寻卡方式= 0x26，寻未进入休眠状态的卡
  * @param  pTagType，卡片类型代码
             = 0x4400，Mifare_UltraLight
             = 0x0400，Mifare_One(S50)
             = 0x0200，Mifare_One(S70)
             = 0x0800，Mifare_Pro(X))
             = 0x4403，Mifare_DESFire
  * @retval 状态值= MI_OK，成功
************************************************************************/
char PcdRequest ( uint8_t ucReq_code, uint8_t * pTagType )
{
  char cStatus;  
  uint8_t ucComMF522Buf [ MAXRLEN ]; 
  uint32_t ulLen;
 
  //清理指示MIFARECyptol单元接通以及所有卡的数据通信被加密的情况
  ClearBitMask ( Status2Reg, 0x08 );
	//发送的最后一个字节的 七位
  WriteRawRC ( BitFramingReg, 0x07 );
  //TX1,TX2管脚的输出信号传递经发送调制的13.56的能量载波信号
  SetBitMask ( TxControlReg, 0x03 );	
 
  ucComMF522Buf [ 0 ] = ucReq_code;		//存入 卡片命令字
 
  cStatus = PcdComMF522 ( PCD_TRANSCEIVE,	
                          ucComMF522Buf,
                          1, 
                          ucComMF522Buf,
                          & ulLen );	//寻卡  
 
  if ( ( cStatus == MI_OK ) && ( ulLen == 0x10 ) )	//寻卡成功返回卡类型 
  {    
     * pTagType = ucComMF522Buf [ 0 ];
     * ( pTagType + 1 ) = ucComMF522Buf [ 1 ];
  }
 
  else
   cStatus = MI_ERR;
 
  return cStatus;	 
}

/************************************
  * @brief  防冲撞
  * @param  pSnr，卡片序列号，4字节
  * @retval 状态值= MI_OK，成功
************************************/
char PcdAnticoll ( uint8_t * pSnr )
{
  char cStatus;
  uint8_t uc, ucSnr_check = 0;
  uint8_t ucComMF522Buf [ MAXRLEN ]; 
  uint32_t ulLen;
  
  //清MFCryptol On位 只有成功执行MFAuthent命令后，该位才能置位
  ClearBitMask ( Status2Reg, 0x08 );
  //清理寄存器 停止收发
  WriteRawRC ( BitFramingReg, 0x00);	
	//清ValuesAfterColl所有接收的位在冲突后被清除
  ClearBitMask ( CollReg, 0x80 );			  
 
  ucComMF522Buf [ 0 ] = 0x93;	          //卡片防冲突命令
  ucComMF522Buf [ 1 ] = 0x20;
 
  cStatus = PcdComMF522 ( PCD_TRANSCEIVE, 
                          ucComMF522Buf,
                          2, 
                          ucComMF522Buf,
                          & ulLen);      //与卡片通信
 
  if ( cStatus == MI_OK)		            //通信成功
  {
    for ( uc = 0; uc < 4; uc ++ )
    {
       * ( pSnr + uc )  = ucComMF522Buf [ uc ]; //读出UID
       ucSnr_check ^= ucComMF522Buf [ uc ];
    }
    
    if ( ucSnr_check != ucComMF522Buf [ uc ] )
      cStatus = MI_ERR;    				 
  }
  
  SetBitMask ( CollReg, 0x80 );
      
  return cStatus;		
}

/**
  * @brief  用RC522计算CRC16
  * @param  pIndata，计算CRC16的数组
  * @param  ucLen，计算CRC16的数组字节长度
  * @param  pOutData，存放计算结果存放的首地址
  * @retval 无
  */
void CalulateCRC ( uint8_t * pIndata, 
                 uint8_t ucLen, 
                 uint8_t * pOutData )
{
  uint8_t uc, ucN;
 
  ClearBitMask(DivIrqReg,0x04);
 
  WriteRawRC(CommandReg,PCD_IDLE);
 
  SetBitMask(FIFOLevelReg,0x80);
 
  for ( uc = 0; uc < ucLen; uc ++)
    WriteRawRC ( FIFODataReg, * ( pIndata + uc ) );   
 
  WriteRawRC ( CommandReg, PCD_CALCCRC );
 
  uc = 0xFF;
 
  do 
  {
      ucN = ReadRawRC ( DivIrqReg );
      uc --;
  } while ( ( uc != 0 ) && ! ( ucN & 0x04 ) );
  
  pOutData [ 0 ] = ReadRawRC ( CRCResultRegL );
  pOutData [ 1 ] = ReadRawRC ( CRCResultRegM );		
}




/**
  * @brief  选定卡片
  * @param  pSnr，卡片序列号，4字节
  * @retval 状态值= MI_OK，成功
  */
char PcdSelect ( uint8_t * pSnr )
{
  char ucN;
  uint8_t uc;
  uint8_t ucComMF522Buf [ MAXRLEN ]; 
  uint32_t  ulLen;
  
  
  ucComMF522Buf [ 0 ] = PICC_ANTICOLL1;
  ucComMF522Buf [ 1 ] = 0x70;
  ucComMF522Buf [ 6 ] = 0;
 
  for ( uc = 0; uc < 4; uc ++ )
  {
    ucComMF522Buf [ uc + 2 ] = * ( pSnr + uc );
    ucComMF522Buf [ 6 ] ^= * ( pSnr + uc );
  }
  
  CalulateCRC ( ucComMF522Buf, 7, & ucComMF522Buf [ 7 ] );
 
  ClearBitMask ( Status2Reg, 0x08 );
 
  ucN = PcdComMF522 ( PCD_TRANSCEIVE,
                     ucComMF522Buf,
                     9,
                     ucComMF522Buf, 
                     & ulLen );
  
  if ( ( ucN == MI_OK ) && ( ulLen == 0x18 ) )
    ucN = MI_OK;  
  else
    ucN = MI_ERR;    
  
  return ucN;		
}


/**
  * @brief  验证卡片密码
  * @param  ucAuth_mode，密码验证模式= 0x60，验证A密钥，
            密码验证模式= 0x61，验证B密钥
  * @param  uint8_t ucAddr，块地址
  * @param  pKey，密码 
  * @param  pSnr，卡片序列号，4字节
  * @retval 状态值= MI_OK，成功
  */
char PcdAuthState ( uint8_t ucAuth_mode, 
                    uint8_t ucAddr, 
                    uint8_t * pKey,
                    uint8_t * pSnr )
{
  char cStatus;
  uint8_t uc, ucComMF522Buf [ MAXRLEN ];
  uint32_t ulLen;
  
 
  ucComMF522Buf [ 0 ] = ucAuth_mode;
  ucComMF522Buf [ 1 ] = ucAddr;
 
  for ( uc = 0; uc < 6; uc ++ )
    ucComMF522Buf [ uc + 2 ] = * ( pKey + uc );   
 
  for ( uc = 0; uc < 6; uc ++ )
    ucComMF522Buf [ uc + 8 ] = * ( pSnr + uc );   
 
  cStatus = PcdComMF522 ( PCD_AUTHENT,
                          ucComMF522Buf, 
                          12,
                          ucComMF522Buf,
                          & ulLen );
 
  if ( ( cStatus != MI_OK ) || ( ! ( ReadRawRC ( Status2Reg ) & 0x08 ) ) )
    cStatus = MI_ERR;   
    
  return cStatus;
}



/**
  * @brief  写数据到M1卡一块
  * @param  ucAddr，块地址（0-63）。M1卡总共有16个扇区(每个扇区有：3个数据块+1个控制块),共64个块
  * @param  pData，写入的数据，16字节
  * @retval 状态值= MI_OK，成功
  */
char PcdWrite ( uint8_t ucAddr, uint8_t * pData )
{
  char cStatus;
  uint8_t uc, ucComMF522Buf [ MAXRLEN ];
  uint32_t ulLen;
   
  
  ucComMF522Buf [ 0 ] = PICC_WRITE;
  ucComMF522Buf [ 1 ] = ucAddr;
 
  CalulateCRC ( ucComMF522Buf, 2, & ucComMF522Buf [ 2 ] );
 
  cStatus = PcdComMF522 ( PCD_TRANSCEIVE,
                          ucComMF522Buf,
                          4, 
                          ucComMF522Buf,
                          & ulLen );
 
  if ( ( cStatus != MI_OK ) || ( ulLen != 4 ) || 
         ( ( ucComMF522Buf [ 0 ] & 0x0F ) != 0x0A ) )
    cStatus = MI_ERR;   
      
  if ( cStatus == MI_OK )
  {
    //memcpy(ucComMF522Buf, pData, 16);
    for ( uc = 0; uc < 16; uc ++ )
      ucComMF522Buf [ uc ] = * ( pData + uc );  
    
    CalulateCRC ( ucComMF522Buf, 16, & ucComMF522Buf [ 16 ] );
 
    cStatus = PcdComMF522 ( PCD_TRANSCEIVE,
                           ucComMF522Buf, 
                           18, 
                           ucComMF522Buf,
                           & ulLen );
    
    if ( ( cStatus != MI_OK ) || ( ulLen != 4 ) || 
         ( ( ucComMF522Buf [ 0 ] & 0x0F ) != 0x0A ) )
      cStatus = MI_ERR;   
    
  } 	
  return cStatus;		
}


/**
  * @brief  读取M1卡一块数据
  * @param  ucAddr，块地址（0-63）。M1卡总共有16个扇区(每个扇区有：3个数据块+1个控制块),共64个块
  * @param  pData，读出的数据，16字节
  * @retval 状态值= MI_OK，成功
  */
char PcdRead ( uint8_t ucAddr, uint8_t * pData )
{
  char cStatus;
  uint8_t uc, ucComMF522Buf [ MAXRLEN ]; 
  uint32_t ulLen;
  
  ucComMF522Buf [ 0 ] = PICC_READ;
  ucComMF522Buf [ 1 ] = ucAddr;
 
  CalulateCRC ( ucComMF522Buf, 2, & ucComMF522Buf [ 2 ] );
 
  cStatus = PcdComMF522 ( PCD_TRANSCEIVE,
                          ucComMF522Buf,
                          4, 
                          ucComMF522Buf,
                          & ulLen );
 
  if ( ( cStatus == MI_OK ) && ( ulLen == 0x90 ) )
  {
    for ( uc = 0; uc < 16; uc ++ )
      * ( pData + uc ) = ucComMF522Buf [ uc ];   
  }
  
  else
    cStatus = MI_ERR;   
   
  return cStatus;		
}


 
 
/**
  * @brief  命令卡片进入休眠状态
  * @param  无
  * @retval 状态值= MI_OK，成功
  */
char PcdHalt( void )
{
	uint8_t ucComMF522Buf [ MAXRLEN ]; 
	uint32_t  ulLen;
  
 
  ucComMF522Buf [ 0 ] = PICC_HALT;
  ucComMF522Buf [ 1 ] = 0;
	
  CalulateCRC ( ucComMF522Buf, 2, & ucComMF522Buf [ 2 ] );
 	PcdComMF522 ( PCD_TRANSCEIVE,
                ucComMF522Buf,
                4, 
                ucComMF522Buf, 
                & ulLen );
 
  return MI_OK;	
}
 


/**
 * 🟡 L1 架构代码 —— 上电时序有硬依赖:GPIO 时钟必须先于 ODR 赋值、片选和复位必须先
 * 给出"未选中/不复位"的安全电平再配 SPI3;顺序错了现象是"寻卡一直失败"而没有报错
 * @brief  初始化 RC522:配片选/复位 GPIO → 配 SPI3 → 硬复位 → 配成 ISO14443A 并开天线
 * @return 无
 * @note   **必须在任何 PcdXxx() 调用之前执行一次**。
 *         整段是阻塞的(复位脉冲里有 Delay_Us 忙等),按工程惯例在调度器启动前调。
 */
void RC522_Init(void)
{
	GPIO_InitTypeDef gpio = {0};

	// ── 1) 先开 GPIOA/GPIOD 的时钟 ──
	// 必须排在写 ODR 之前:外设时钟没开的时候往 ODR 里写是**写不进去**的
	// (STM32 对外设寄存器的写入会被直接丢掉),电平根本不会变。
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOD, ENABLE);

	// ── 2) 先把两条控制线摆到"安全电平",再配成输出 ──
	//     CS = 高(未选中)、RST = 高(不复位)。
	//     为什么要在 GPIO_Init 之前写:GPIO_Init 只改 MODER/OTYPER/OSPEEDR/PUPDR,
	//     **不碰 ODR**。所以先往 ODR 里写好初值,引脚一变成输出就直接驱动这个电平,
	//     中间不会出现"已经输出、但输出的是复位值 0"的瞬间 ——
	//     那一瞬间 CS 是低电平,RC522 会被误选中,把 SPI3 初始化期间总线上的
	//     杂波当成时钟收进去,后面所有的寄存器读写就全乱了。
	//     (spi1_w25_Config 里 SPI_CS = 1 也是同一个套路)
	RFID_CS_H;
	RFID_RST_H;

	gpio.GPIO_Mode  = GPIO_Mode_OUT;
	gpio.GPIO_OType = GPIO_OType_PP;
	gpio.GPIO_Speed = GPIO_Low_Speed;   // 片选/复位不用高速,低速还少点干扰
	gpio.GPIO_PuPd  = GPIO_PuPd_NOPULL;

	gpio.GPIO_Pin = GPIO_Pin_15;        // PA15 → RC522 SDA(片选)
	GPIO_Init(GPIOA, &gpio);

	gpio.GPIO_Pin = GPIO_Pin_2;         // PD2  → RC522 RST(复位)
	GPIO_Init(GPIOD, &gpio);

	// ── 3) 数据线交给 SPI3 硬件(PC10/PC11/PC12) ──
	spi3_Config();

	// ── 4) 复位 + 配置工作模式 ──
	Reset_RC522();                  // 硬复位 + 关天线再开天线
	M500PcdConfigISOType('A');      // ISO14443A:Mifare One 卡用的就是这套
}












//void print_info(unsigned char *p,int cnt)
//{
//  int i;
//  for(i=0;i<cnt;i++)
//  {
//		if(!i)
//			printf("0x%02X ",p[i]);
//		else
//			printf("%02X ",p[i]);
//  }
//  printf("\r\n");
//}


