#ifndef _LCD_H_
#define _LCD_H_


#include "stm32f4xx.h"
#include "io_bit.h"
#include "delay.h"
#include "w25qxx.h"

#define LCD_BLK PBout(1)
#define LCD_DC PBout(14)
#define LCD_CS PBout(12)
#define LCD_SCK PBout(13)
#define LCD_MOSI PBout(15)
#define LCD_RST PBout(10)

/* 颜色定义开始 RGB(565)*/
#define WHITE        0xFFFF
#define BLACK        0x0000	  
#define BLUE         0x001F  
#define BRED         0XF81F
#define GRED 	       0XFFE0
#define GBLUE		     0X07FF
#define RED          0xF800
#define MAGENTA      0xF81F
#define GREEN        0x07E0
#define CYAN         0x7FFF
#define YELLOW       0xFFE0
#define BROWN 			 0XBC40 //棕色
#define BRRED 			 0XFC07 //棕红色
#define GRAY  			 0X8430 //灰色
#define DARKBLUE     0X01CF	//深蓝色
#define LIGHTBLUE    0X7D7C	//浅蓝色  
#define GRAYBLUE     0X5458 //灰蓝色
#define LIGHTGREEN   0X841F //浅绿色
#define LGRAY 			 0XC618 //浅灰色(PANNEL),窗体背景色
#define LGRAYBLUE    0XA651 //浅灰蓝色(中间层颜色)
#define LBBLUE       0X2B12 //浅棕蓝色(选择条目的反色)
/* 颜色定义结束 */


void LCD_Pin_Config(void);
void Spi_Send_byte(u8 data);
void LCD_Send_Cmd(u8 cmd);
void LCD_Send_Data(u8 data);
void LCD_Send_16bit(u16 data);
void LCD_Set_Region(u16 row_s,u16 row_e,u16 col_s,u16 col_e);
void LCD_Clear(u16 row_s,u16 row_e,u16 col_s,u16 col_e,u16 color);
void LCD_Init(void);   /* 内部顺带创建 LCD 互斥量,必须在任何画屏动作之前调用 */

/* LCD 互斥量 —— 见 lcd.c 顶部的大段说明。
 * 底层的 LCD_Set_Point/LCD_Clear/LCD_Dis_Pic/LCD_FillRectangle 已经各自
 * 自动上锁,所以"画面会不会串位"不用调用方操心。
 * 调用方只在需要**整段画面作为一个事务**(不希望别的任务把它切成两半)时才
 * 手动 LCD_Lock()/LCD_Unlock(),例如 lock.c 的 lock_lcd_text()。
 * 互斥量是递归的,已经自动上锁的函数里再套一层不会死锁。
 * ⚠ 调度器启动之前这两个函数是空操作,可以直接调。 */
void LCD_Lock(void);
void LCD_Unlock(void);
void LCD_Set_Point(u16 row,u16 col,u16 color);



void LCD_DrawLine(u16 row1,u16 col1,u16 row2,u16 col2,u16 color);
void LCD_DrawCircle(u16 row,u16 col,u16 r,u16 color);
void LCD_FillCircle(u16 row,u16 col,u16 r,u16 color);
void LCD_DrawRectangle(u16 row1,u16 col1,u16 row2,u16 col2,u16 color);
void LCD_FillRectangle(u16 row1,u16 col1,u16 row2,u16 col2,u16 color);
void LCD_DrawTriangle(u16 row1,u16 col1,u16 row2,u16 col2,u16 row3,u16 col3,u16 color);
void LCD_FillTriangle(u16 row1,u16 col1,u16 row2,u16 col2,u16 row3,u16 col3,u16 color);


void LCD_Dis_Ascii(u16 row,u16 col,u8 font_size,u16 font_color,u16 back_color,u8 *buff);
void LCD_Dis_Font(u16 row,u16 col,u8 font_size,u16 font_color,u16 back_color,u8 *buff);
void LCD_Font_Dis_Ascii(u16 row,u16 col,u8 font_size,u16 font_color,u16 back_color,u8 asc);
void LCD_Font_Dis_Font(u16 row,u16 col,u8 font_size,u16 font_color,u16 back_color,u8 *font);
void LCD_Font_Dis(uint16_t row, uint16_t col, uint8_t font_size, uint16_t font_color, uint16_t back_color, uint8_t *font);
void LCD_Roll_Dis(u16 row,u8 font_size,u16 font_color,u16 back_color,u8 *str,u8 speed);




void LCD_printf_font(uint16_t row, uint16_t col, uint8_t font_size,const uint16_t *data, uint16_t font_color,uint16_t back_color);

void LCD_Dis_Pic(uint16_t row,uint16_t col,const uint8_t *pic);


#endif

