#include "debug.h"

void font_test(void)
{
    //下载字库
    DMA_Font_Config();
    Font_Load();

    W25Qxx_Read_Bytes(0, font.buff[0], BUFFSIZE); //读取字库
    for(int i = 0; i < BUFFSIZE; i++)
    {
        printf("%02x ", font.buff[0][i]);
    }

    /*
    格式串里那个 0 是"补零标志"，但它只负责把位数补到指定的宽度——你后面没写宽度，所以它什么都不干，输出的是十六进制的最短形式（0 就输出 0）。

    %02x 的三个部分：


    %  0   2   x
    │  │   │   └─ 十六进制小写 (大写用 X)
    │  │   └───── 最小宽度 2 位，不足补零
    │  └───────── 用 0 补位（而不是空格）
    └──────────── 格式说明符开始
    */
}

