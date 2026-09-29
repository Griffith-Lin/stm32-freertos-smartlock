#ifndef RFID_H
#define RFID_H

// ⚠ 原厂移植过来的这个头文件**一个 include 都没有**,而下面的引脚宏
//   用的是 PAout()/PDout() 这类位带写法 —— 不把 stm32f4xx.h 和 bitband.h
//   带进来,它们就是未定义符号,整个文件编译不过。这是它第一次进本工程
//   必须补的第一件事。
#include "stm32f4xx.h"   // uint8_t、GPIO 寄存器定义
#include "bitband.h"     // PAout()/PCout()/PDout()/PCin()


//MF522命令字

#define PCD_IDLE              0x00               //取消当前命令
#define PCD_AUTHENT           0x0E               //验证密钥
#define PCD_RECEIVE           0x08               //接收数据
#define PCD_TRANSMIT          0x04               //发送数据
#define PCD_TRANSCEIVE        0x0C               //发送并接收数据
#define PCD_RESETPHASE        0x0F               //复位
#define PCD_CALCCRC           0x03               //CRC计算
 

//Mifare_One卡片命令字

#define PICC_REQIDL           0x26               //寻天线区内未进入休眠状态
#define PICC_REQALL           0x52               //寻天线区内全部卡
#define PICC_ANTICOLL1        0x93               //防冲撞
#define PICC_ANTICOLL2        0x95               //防冲撞
#define PICC_AUTHENT1A        0x60               //验证A密钥
#define PICC_AUTHENT1B        0x61               //验证B密钥
#define PICC_READ             0x30               //读块
#define PICC_WRITE            0xA0               //写块
#define PICC_DECREMENT        0xC0               //扣款
#define PICC_INCREMENT        0xC1               //充值
#define PICC_RESTORE          0xC2               //调块数据到缓冲区
#define PICC_TRANSFER         0xB0               //保存缓冲区中数据
#define PICC_HALT             0x50               //休眠
 

//MF522 FIFO长度定义

#define DEF_FIFO_LENGTH       64                 //FIFO size=64byte
#define MAXRLEN  18
 

//MF522寄存器定义

// PAGE 0
#define     RFU00                 0x00    
#define     CommandReg            0x01    
#define     ComIEnReg             0x02    
#define     DivlEnReg             0x03    
#define     ComIrqReg             0x04    
#define     DivIrqReg             0x05
#define     ErrorReg              0x06    
#define     Status1Reg            0x07    
#define     Status2Reg            0x08    
#define     FIFODataReg           0x09
#define     FIFOLevelReg          0x0A
#define     WaterLevelReg         0x0B
#define     ControlReg            0x0C
#define     BitFramingReg         0x0D
#define     CollReg               0x0E
#define     RFU0F                 0x0F
// PAGE 1     
#define     RFU10                 0x10
#define     ModeReg               0x11
#define     TxModeReg             0x12
#define     RxModeReg             0x13
#define     TxControlReg          0x14
#define     TxAutoReg             0x15
#define     TxSelReg              0x16
#define     RxSelReg              0x17
#define     RxThresholdReg        0x18
#define     DemodReg              0x19
#define     RFU1A                 0x1A
#define     RFU1B                 0x1B
#define     MifareReg             0x1C
#define     RFU1D                 0x1D
#define     RFU1E                 0x1E
#define     SerialSpeedReg        0x1F
// PAGE 2    
#define     RFU20                 0x20  
#define     CRCResultRegM         0x21
#define     CRCResultRegL         0x22
#define     RFU23                 0x23
#define     ModWidthReg           0x24
#define     RFU25                 0x25
#define     RFCfgReg              0x26
#define     GsNReg                0x27
#define     CWGsCfgReg            0x28
#define     ModGsCfgReg           0x29
#define     TModeReg              0x2A
#define     TPrescalerReg         0x2B
#define     TReloadRegH           0x2C
#define     TReloadRegL           0x2D
#define     TCounterValueRegH     0x2E
#define     TCounterValueRegL     0x2F
// PAGE 3      
#define     RFU30                 0x30
#define     TestSel1Reg           0x31
#define     TestSel2Reg           0x32
#define     TestPinEnReg          0x33
#define     TestPinValueReg       0x34
#define     TestBusReg            0x35
#define     AutoTestReg           0x36
#define     VersionReg            0x37
#define     AnalogTestReg         0x38
#define     TestDAC1Reg           0x39  
#define     TestDAC2Reg           0x3A   
#define     TestADCReg            0x3B   
#define     RFU3C                 0x3C   
#define     RFU3D                 0x3D   
#define     RFU3E                 0x3E   
#define     RFU3F		  		        0x3F
 

//和MF522通讯时返回的错误代码

#define 	MI_OK                 0x26
#define 	MI_NOTAGERR           0xcc
#define 	MI_ERR                0xbb
 
 
/*─────────────────────────── RC522 引脚 ───────────────────────────*/
//
//  接线(引脚只在这一个地方出现,换板子只改这一段):
//      PC10  SPI3_SCK   → RC522 的 SCK
//      PC11  SPI3_MISO  → RC522 的 MISO
//      PC12  SPI3_MOSI  → RC522 的 MOSI
//      PA15  普通输出    → RC522 的 SDA(片选 CS,低电平选中)
//      PD2   普通输出    → RC522 的 RST(复位,低电平有效)
//
//  ⚠ SCK/MISO/MOSI **故意没有宏**,也不该再加回来:
//    原厂代码是拿 PCout()/PCin() 一位一位翻软件 SPI 的,那三个宏在本工程里
//    是**有害**的 —— PC10/PC12 已经被 spi3_Config() 配成 SPI3 的复用功能(AF6),
//    再往 ODR 里写电平等于绕过外设直接拽引脚,和 SPI3 自己输出的波形对着干。
//    数据线现在整条交给 SPI3 硬件,只有"片选"和"复位"这两个属于 RC522
//    器件自己的脚,才需要手工控制。
//
//  ⚠ PA15 兼着 JTDI,用之前得知道代价:
//    STM32F4 不像 F1 有 SWJ_CFG 寄存器,把 MODER 配成普通输出就等于把引脚
//    从 JTAG 手里抢回来 —— 工程里 PB3/PB4(JTDO/NJTRST)当电机脚用,走的是
//    同一条路。代价只是 **JTAG 没了,SWD 不受影响**;本工程用 cmsis-dap 走
//    SWD 下载调试,所以没影响。
#define RFID_CS_H   (PAout(15) = 1)
#define RFID_CS_L   (PAout(15) = 0)

#define RFID_RST_H  (PDout(2) = 1)
#define RFID_RST_L  (PDout(2) = 0)


 
/*********************************** 函数 *********************************************/

/**
 * 🟡 L1 架构代码 —— 整个模块唯一的初始化入口,依赖 SPI3 外设和 Delay_Us;
 * 里面写的是 RC522 的"上电时序 + 工作模式",配错的表现是"寻卡一直失败"而没有报错
 * @brief  初始化 RC522:配 SPI3 → 硬复位 → 配成 ISO14443A 工作模式并开天线
 * @return 无
 * @note   **必须在任何 PcdXxx() 之前调用一次**。调用它之前 PA15/PD2 只是普通
 *         GPIO,SPI3 也还没使能。
 *         它是**阻塞**的(复位脉冲和天线开启里面都有 Delay_Us 忙等),所以按
 *         工程惯例要赶在 vTaskStartScheduler() 之前调 —— 见 main.c。
 */
void RC522_Init(void);

char PcdRequest ( uint8_t ucReq_code, uint8_t * pTagType );
char PcdAnticoll ( uint8_t * pSnr );
char PcdSelect ( uint8_t * pSnr );
char PcdAuthState ( uint8_t ucAuth_mode,uint8_t ucAddr,uint8_t * pKey,uint8_t * pSnr );
char PcdWrite ( uint8_t ucAddr, uint8_t * pData );
char PcdRead ( uint8_t ucAddr, uint8_t * pData );
char PcdHalt( void );






 
#endif
