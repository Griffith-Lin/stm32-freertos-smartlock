#include "lcd.h"

/*******************
 * 函数功能：LCD管脚初始化
 * 形参：无
 * 返回值：无
 * 函数描述：
LCD_RES     ---     PB10   ---   通用推挽输出
LCD_MOSI     ---     PB15   ---   通用推挽输出
LCD_SCK     ---     PB13   ---   通用推挽输出
LCD_CS      ---     PB12   ---   通用推挽输出
LCD_DC      ---     PB14   ---   通用推挽输出
LCD_BLK     ---     PB1    ---   通用推挽输出
*******************/
void LCD_Pin_Config(void)
{
	
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE); // 打开PB时钟
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_10 | GPIO_Pin_15 | GPIO_Pin_13 | GPIO_Pin_12 | GPIO_Pin_14 | GPIO_Pin_1;
    GPIO_Init(GPIOB, &GPIO_InitStruct);

	LCD_CS = 1;


	LCD_RST = 0;   // ① 拉低：让芯片彻底复位（清掉上电瞬间的不确定状态）
	Delay_Ms(50);  //    保持至少几十 ms（芯片要求的最小复位脉宽）
	LCD_RST = 1;   // ② 拉高：释放复位
	Delay_Ms(120); //    芯片要求释放后等 120ms 内部稳定，再发初始化命令
	// 为什么建议先拉低再拉高，而不是直接拉高？因为：

	// 上电瞬间电源电压还没稳定，芯片内部逻辑可能处于随机状态
	// 主动拉低 → 等电源稳定 → 再释放，能保证芯片从一个确定的干净起点开始跑
	// 你之前只拉高（没有先拉低的过程）也能工作，是因为模块板载电路通常已经做过一次上电自复位，你只是把单片机误按住的低电平放开而已——效果等同于"释放复位"
}

/*
Function name:Spi_Send_byte
Description:SPI的模拟代码
param:data---待发送的数据
retval:None
Remarks:
*/
void Spi_Send_byte(uint8_t data)
{
	uint8_t i;
	for (i = 0; i < 8; i++)
	{
		LCD_SCK = 0;
		if (data & (0x80 >> i))
		{
			LCD_MOSI = 1;
		}
		else
		{
			LCD_MOSI = 0;
		}
		LCD_SCK = 1;
	}
}

/*
Function name:LCD_Send_Cmd
Description:SPI发送命令
param:cmd---待发送的命令
retval:None
Remarks:
*/
void LCD_Send_Cmd(uint8_t cmd)
{
	LCD_CS = 0;
	LCD_DC = 0;
	Spi_Send_byte(cmd);
	LCD_CS = 1;
}

/*
Function name:LCD_Send_Data
Description:SPI发送数据
param:data---待发送的数据
retval:None
Remarks:
*/
void LCD_Send_Data(uint8_t data)
{
	LCD_CS = 0;
	LCD_DC = 1;
	Spi_Send_byte(data);
	LCD_CS = 1;
}

/*
Function name:LCD_Send_16bit
Description:SPI发送16bit数据
param:data---待发送的数据
retval:None
Remarks:
*/
void LCD_Send_16bit(uint16_t data)
{
	LCD_CS = 0;
	LCD_DC = 1;
	Spi_Send_byte(data >> 8);
	Spi_Send_byte(data);
	LCD_CS = 1;
}

/*
Function name:LCD_Set_Region
Description:设置写入的区域
param:row_s,row_e--起始/终止行  col_s,col_e--起始/终止列
retval:None
Remarks:
*/
void LCD_Set_Region(uint16_t row_s, uint16_t row_e, uint16_t col_s, uint16_t col_e)
{
	LCD_Send_Cmd(0x2B); // 设置行
	LCD_Send_16bit(row_s);
	LCD_Send_16bit(row_e - 1);

	LCD_Send_Cmd(0x2A); // 设置列
	LCD_Send_16bit(col_s);
	LCD_Send_16bit(col_e - 1);

	LCD_Send_Cmd(0x2C); // 允许写入
}

/*
Function name:LCD_Clear
Description:清屏函数
param:row_s,row_e--起始/终止行  col_s,col_e--起始/终止列  color--清屏颜色
retval:None
Remarks:
*/
void LCD_Clear(uint16_t row_s, uint16_t row_e, uint16_t col_s, uint16_t col_e, uint16_t color)
{
	u32 i, j;
	LCD_Set_Region(row_s, row_e, col_s, col_e);
	//	for(i=row_s;i<=row_e;i++)//行
	//	{
	//		for(j=col_s;j<=col_e;j++)//列
	//		{
	//			LCD_Send_16bit(color);
	//		}
	//	}
	for (i = 0; i <= (row_e - row_s) * (col_e - col_s); i++)
	{
		LCD_Send_16bit(color);
	}
}

// 初始化函数
void LCD_Init(void)
{
	LCD_Pin_Config();
	//	LCD_Set_Brightness(84,1000);//屏幕亮度调节
	/* 复位ST7789VM驱动器 */
	//	LCD_RESET=0;
	//	Delay_ms(100);
	//	LCD_RESET=1;
	Delay_Ms(100);

	/* start initial sequence */
	LCD_Send_Cmd(0x36);
	LCD_Send_Data(0x00);

	LCD_Send_Cmd(0x3A);
	LCD_Send_Data(0x05);

	LCD_Send_Cmd(0xB2);
	LCD_Send_Data(0x0C);
	LCD_Send_Data(0x0C);
	LCD_Send_Data(0x00);
	LCD_Send_Data(0x33);
	LCD_Send_Data(0x33);

	LCD_Send_Cmd(0xB7);
	LCD_Send_Data(0x35);

	LCD_Send_Cmd(0xBB);
	LCD_Send_Data(0x19);

	LCD_Send_Cmd(0xC0);
	LCD_Send_Data(0x2C);

	LCD_Send_Cmd(0xC2);
	LCD_Send_Data(0x01);

	LCD_Send_Cmd(0xC3);
	LCD_Send_Data(0x12);

	LCD_Send_Cmd(0xC4);
	LCD_Send_Data(0x20);

	LCD_Send_Cmd(0xC6);
	LCD_Send_Data(0x0F);

	LCD_Send_Cmd(0xD0);
	LCD_Send_Data(0xA4);
	LCD_Send_Data(0xA1);

	LCD_Send_Cmd(0xE0);
	LCD_Send_Data(0xD0);
	LCD_Send_Data(0x04);
	LCD_Send_Data(0x0D);
	LCD_Send_Data(0x11);
	LCD_Send_Data(0x13);
	LCD_Send_Data(0x2B);
	LCD_Send_Data(0x3F);
	LCD_Send_Data(0x54);
	LCD_Send_Data(0x4C);
	LCD_Send_Data(0x18);
	LCD_Send_Data(0x0D);
	LCD_Send_Data(0x0B);
	LCD_Send_Data(0x1F);
	LCD_Send_Data(0x23);

	LCD_Send_Cmd(0xE1);
	LCD_Send_Data(0xD0);
	LCD_Send_Data(0x04);
	LCD_Send_Data(0x0C);
	LCD_Send_Data(0x11);
	LCD_Send_Data(0x13);
	LCD_Send_Data(0x2C);
	LCD_Send_Data(0x3F);
	LCD_Send_Data(0x44);
	LCD_Send_Data(0x51);
	LCD_Send_Data(0x2F);
	LCD_Send_Data(0x1F);
	LCD_Send_Data(0x1F);
	LCD_Send_Data(0x20);
	LCD_Send_Data(0x23);

	LCD_Send_Cmd(0x21);
	LCD_Send_Cmd(0x11);
	LCD_Send_Cmd(0x29);

	LCD_Clear(0, 320, 0, 240, WHITE); /* 先清屏(行0~319,列0~239) */

	/* 开启背光 */
	LCD_BLK = 1;
	//	TIM_SetCompare4(TIM3,1000);//PWM1 CNT<CCR有效 有效为高 CCR越大屏幕越亮
}

// 打印字模
void LCD_printf_font(uint16_t row, uint16_t col, uint8_t font_size, const uint16_t *data, uint16_t font_color, uint16_t back_color)
{
	uint8_t wide;
	uint8_t i, j, k;
	wide = font_size / 8; // 转为字节

	for (i = 0; i < font_size; i++) // 行
	{
		for (j = 0; j < wide; j++) // 这个字的第i行的第几个字节
		{
			for (k = 0; k < 8; k++) // 把字节拆成位
			{
				if (data[i * wide + j] & (0x80 >> k))
				{
					LCD_Set_Point(row + i, col + k + 8 * j, font_color);
				}
				else
				{
					LCD_Set_Point(row + i, col + k + 8 * j, back_color);
				}
			}
		}
	}
}

/*
Function name:LCD_Set_Point
Description:打点函数
param:row,col--点的位置  color--颜色
retval:None
Remarks:
*/

void LCD_Set_Point(uint16_t row, uint16_t col, uint16_t color)
{
	LCD_Set_Region(row, row + 1, col, col + 1); // 一个像素 = [row,row+1) × [col,col+1)
	LCD_Send_16bit(color);
}

// 图片显示函数
/*
数据头如下
typedef struct _HEADCOLOR
{
   unsigned char scan;
   unsigned char gray;
   unsigned short w;
   unsigned short h;
   unsigned char is565;
   unsigned char rgb;
}HEADCOLOR;
*/
void LCD_Dis_Pic(uint16_t row, uint16_t col, const uint8_t *pic)
{
	u32 i;
	uint16_t w, h;
	w = pic[2] << 8 | pic[3];
	h = pic[4] << 8 | pic[5];
	LCD_Set_Region(row, row + h, col, col + w);
	for (i = 0; i <= h * w - 1; i++)
	{
		LCD_Send_16bit(pic[8 + 2 * i] << 8 | pic[9 + 2 * i]);
	}
}

/*
Function name:LCD_Font_Dis_Ascii
Description:字符显示函数---字库支持
param:row,col--左上角坐标  font_size--字体大小  font_color--字体颜色  back_color--背景颜色  asc--想要显示的字符
retval:None
Remarks:
*/
void LCD_Font_Dis_Ascii(uint16_t row, uint16_t col, uint8_t font_size, uint16_t font_color, uint16_t back_color, uint8_t asc)
{
	uint16_t i, j, k;
	uint8_t width = 0;
	uint8_t buff[100]; // 最少给64字节
	u32 addr = 0;
	width = font_size / 2 / 8; // 算 ASCII 字符字模"每行点阵"占几个字节(width 的单位是字节,不是像素)
	if (font_size / 2 % 8)
	{
		width += 1;
	}
	switch (font_size)
	{
	// 选择对应字库的首地址
	case 16:
		addr += 0x00000000;
		break;
	case 24:
		addr += 0x0004064C;
		break;
	case 32:
		addr += 0x000D1A68;
		break;
	}
	addr = addr + asc * font_size * width;//左边的 addr 来自上面的 switch:16/24/32 号字字模区在 flash 的起始地址(0x00000000 / 0x0004064C / 0x000D1A68)
//asc 就是 ASCII 码,比如 'A' = 65,'Q' = 81
//每个字符占用的字节数 = font_size * width —— 高 font_size 行 × 每行 width 字节(就是刚才算的向上取整后的行宽)
	W25Qxx_Read_Bytes(addr, buff, font_size * width);//读 font_size * width 个字节

	for (i = 0; i < font_size; i++) // 行（单位：像素）
	{
		for (j = 0; j < width; j++) // 列（单位：字节）
		{
			for (k = 0; k < 8; k++) // 把字节拆成位
			{
				if (buff[i * width + j] & (0x80 >> k))
				{
					LCD_Set_Point(row + i, col + k + 8 * j, font_color);
				}
				else
				{
					LCD_Set_Point(row + i, col + k + 8 * j, back_color);
				}
			}
		}
	}
}

/*
Function name:LCD_Font_Dis_Font
Description:汉字显示函数---需要字库支持
param:row,col--左上角坐标  font_size--字体大小  font_color--字体颜色  back_color--背景颜色  font--需要显示的汉字
retval:None
Remarks:
*/
void LCD_Font_Dis_Font(uint16_t row, uint16_t col, uint8_t font_size, uint16_t font_color, uint16_t back_color, uint8_t *font)
{
	uint16_t i, j, k;
	uint8_t width = 0;
	uint8_t buff[200]; // 最少给128字节
	u32 addr = 0;
	width = font_size / 8;
	switch (font_size)
	{
	case 16:
		addr += 0x00000806;
		break;
	case 24:
		addr += 0x00041E52;
		break;
	case 32:
		addr += 0x000D3A6E;
		break;
	}
	addr = addr + ((font[0] - 0xA1) * 94 + (font[1] - 0xA1)) * font_size * width;
	W25Qxx_Read_Bytes(addr, buff, font_size * width);

	for (i = 0; i < font_size; i++) // 行
	{
		for (j = 0; j < width; j++) // 列
		{
			for (k = 0; k < 8; k++) // 把字节拆成位
			{
				if (buff[i * width + j] & (0x80 >> k))
				{
					LCD_Set_Point(row + i, col + k + 8 * j, font_color);
				}
				else
				{
					LCD_Set_Point(row + i, col + k + 8 * j, back_color);
				}
			}
		}
	}
}

//中英文可混写
void LCD_Font_Dis(uint16_t row, uint16_t col, uint8_t font_size, uint16_t font_color, uint16_t back_color, uint8_t *font)
{
    while(*font!='\0')
    {             
        if(*font<0xa1)//gb2312的最小区
        {
        LCD_Font_Dis_Ascii(row,col,font_size,font_color,back_color,*font);   
        col+=font_size/2;
        font++;
        }
        else
        {
        LCD_Font_Dis_Font(row,col,font_size,font_color,back_color,font);
        col+=font_size;
        font+=2;
        }
    }
    //判断最后一位是什么英文还是中文，将col还原成真实结尾
}


//void LCD_Font_roll(uint16_t row, uint16_t col, uint8_t font_size, uint16_t font_color, uint16_t back_color, uint8_t *font)
//{
//    uint16_t row_tmp=row;
//    uint16_t col_tmp=col;
//    
//    LCD_Font_Dis(row_tmp,col_tmp,font_size,font_color,back_color,font);
//    
//}






/*
Function name:LCD_Dis_Str_Roll
Description:字符串显示函数---支持滚屏显示的
param:row,col--左上角坐标  font_size--字体大小  font_color--字体颜色  back_color--背景颜色  str--需要显示的字符串首地址
retval:None
Remarks:
*/
void LCD_Dis_Str_Roll(u16 row,u16 col,u8 font_size,u16 font_color,u16 back_color,u8 *str)
{
	u8 width=0;
	width=font_size/2/8;
	if(font_size/2%8)
	{
		width+=1;
	}
	while(*str!='\0')
	{
		if(*str>0xa0)//汉字
		{
//			if(col>240-font_size)
//			{
//				row+=font_size;
//				col=0;
//			}
			LCD_Font_Dis_Font(row,col,font_size,font_color,back_color,str);
			str+=2;
			col+=font_size;
		}
		else//字符
		{
//			if(col>240-width*8)
//			{
//				row+=font_size;
//				col=0;
//			}
			LCD_Font_Dis_Ascii(row,col,font_size,font_color,back_color,*str);
			str+=1;
			col+=width*8;
		}
	}
}


/*
Function name:LCD_Roll_Dis
Description:滚动显示---需要字库支持
param:row--行  font_size--字体大小  font_color--字体颜色  back_color--背景颜色  str--需要显示的字符串首地址 speed--速度
retval:None
Remarks:
*/
void LCD_Roll_Dis(u16 row,u8 font_size,u16 font_color,u16 back_color,u8 *str,u8 speed)
{
	u16 col=240;
	while(1)
	{
		LCD_Dis_Str_Roll(row,col,font_size,font_color,back_color,str);
		if(col==0)
		{
			col=240;
			LCD_Clear(row,row+font_size,0,240,back_color);
		}
		col--;
		Delay_Ms(speed);
	}
}




















/* ==================== 画图函数(参考正点原子 lcd.c) ==================== */

/*
Function name: LCD_DrawLine
Description: 画直线,Bresenham算法,整数运算无浮点
param:row1,col1--起点  row2,col2--终点  color--颜色
*/
void LCD_DrawLine(uint16_t row1, uint16_t col1, uint16_t row2, uint16_t col2, uint16_t color)
{
	uint16_t t;
	int xerr = 0, yerr = 0, delta_x, delta_y, distance;
	int incx, incy, uRow, uCol;

	delta_x = col2 - col1; /* x方向增量 */
	delta_y = row2 - row1; /* y方向增量 */
	uRow = row1;
	uCol = col1;

	if (delta_x > 0)
		incx = 1;
	else if (delta_x == 0)
		incx = 0;
	else
	{
		incx = -1;
		delta_x = -delta_x;
	}

	if (delta_y > 0)
		incy = 1;
	else if (delta_y == 0)
		incy = 0;
	else
	{
		incy = -1;
		delta_y = -delta_y;
	}

	if (delta_x > delta_y)
		distance = delta_x;
	else
		distance = delta_y;

	for (t = 0; t <= distance + 1; t++)
	{
		LCD_Set_Point(uRow, uCol, color);
		xerr += delta_x;
		yerr += delta_y;
		if (xerr > distance)
		{
			xerr -= distance;
			uCol += incx;
		}
		if (yerr > distance)
		{
			yerr -= distance;
			uRow += incy;
		}
	}
}

/*
Function name: LCD_DrawCircle
Description: 画圆,中点画圆法,利用8对称性
param:row,col--圆心  r--半径  color--颜色
*/
void LCD_DrawCircle(uint16_t row, uint16_t col, uint16_t r, uint16_t color)
{
	int x = 0, y = r;
	int d = 3 - 2 * r; /* 中点判别式初值 */

	while (x <= y)
	{
		/* 8个对称点,只算1/8圆 */
		LCD_Set_Point(row - y, col + x, color);
		LCD_Set_Point(row - x, col + y, color);
		LCD_Set_Point(row + x, col + y, color);
		LCD_Set_Point(row + y, col + x, color);
		LCD_Set_Point(row + y, col - x, color);
		LCD_Set_Point(row + x, col - y, color);
		LCD_Set_Point(row - x, col - y, color);
		LCD_Set_Point(row - y, col - x, color);

		if (d < 0)
			d += 4 * x + 6; /* 中点在圆内 */
		else
		{
			d += 4 * (x - y) + 10;
			y--;
		} /* 中点在圆外 */
		x++;
	}
}

/*
Function name: LCD_DrawRectangle
Description: 画矩形(边框),四条边各画一条线
*/
void LCD_DrawRectangle(uint16_t row1, uint16_t col1, uint16_t row2, uint16_t col2, uint16_t color)
{
	LCD_DrawLine(row1, col1, row1, col2, color);
	LCD_DrawLine(row2, col1, row2, col2, color);
	LCD_DrawLine(row1, col1, row2, col1, color);
	LCD_DrawLine(row1, col2, row2, col2, color);
}

/*
Function name: LCD_DrawTriangle
Description: 画三角形(边框),三条边
*/
void LCD_DrawTriangle(uint16_t row1, uint16_t col1, uint16_t row2, uint16_t col2, uint16_t row3, uint16_t col3, uint16_t color)
{
	LCD_DrawLine(row1, col1, row2, col2, color);
	LCD_DrawLine(row2, col2, row3, col3, color);
	LCD_DrawLine(row3, col3, row1, col1, color);
}

/*
Function name: LCD_FillRectangle
Description: 填充矩形,一次设置窗口批量刷,比逐点快得多
*/
void LCD_FillRectangle(uint16_t row1, uint16_t col1, uint16_t row2, uint16_t col2, uint16_t color)
{
	u32 i;
	LCD_Set_Region(row1, row2, col1, col2); /* 与 LCD_Clear 相同语义 */
	for (i = 0; i < (u32)(row2 - row1) * (col2 - col1); i++)
		LCD_Send_16bit(color);
}

/*
Function name: LCD_FillCircle
Description: 填充圆,逐行画水平弦(整数运算,无sqrt)
*/
void LCD_FillCircle(uint16_t row, uint16_t col, uint16_t r, uint16_t color)
{
	int x = 0, y = r;
	int d = 3 - 2 * r;

	while (x <= y)
	{
		/* 每对对称点之间填一条水平线,4条弦覆盖整个圆 */
		LCD_DrawLine(row - y, col - x, row - y, col + x, color);
		LCD_DrawLine(row + y, col - x, row + y, col + x, color);
		LCD_DrawLine(row - x, col - y, row - x, col + y, color);
		LCD_DrawLine(row + x, col - y, row + x, col + y, color);

		if (d < 0)
			d += 4 * x + 6;
		else
		{
			d += 4 * (x - y) + 10;
			y--;
		}
		x++;
	}
}

/*
Function name: LCD_FillTriangle
Description: 填充三角形,扫描线法:逐行求与三条边的交点,取左右边界填充
*/
void LCD_FillTriangle(uint16_t row1, uint16_t col1, uint16_t row2, uint16_t col2, uint16_t row3, uint16_t col3, uint16_t color)
{
	int y, xl, xr, x, i;
	int minr = row1, maxr = row1;
	/* 三条边 */
	int edges[3][4] = {
		{row1, col1, row2, col2},
		{row2, col2, row3, col3},
		{row3, col3, row1, col1}};

	if (row2 < minr)
		minr = row2;
	if (row2 > maxr)
		maxr = row2;
	if (row3 < minr)
		minr = row3;
	if (row3 > maxr)
		maxr = row3;

	for (y = minr; y <= maxr; y++)
	{
		xl = 32767;
		xr = -32768;
		for (i = 0; i < 3; i++)
		{
			int r1 = edges[i][0], c1 = edges[i][1];
			int r2 = edges[i][2], c2 = edges[i][3];
			/* 半开区间判跨,避免顶点被重复计入 */
			if ((r1 <= y && r2 > y) || (r2 <= y && r1 > y))
			{
				x = c1 + (c2 - c1) * (y - r1) / (r2 - r1);
				if (x < xl)
					xl = x;
				if (x > xr)
					xr = x;
			}
		}
		if (xr >= xl)
			LCD_DrawLine((uint16_t)y, (uint16_t)xl, (uint16_t)y, (uint16_t)xr, color);
	}
}
