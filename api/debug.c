#include "debug.h"

void font_test(void)
{
    // 下载字库
    DMA_Font_Config();
    Font_Load();

    W25Qxx_Read_Bytes(0, font.buff[0], BUFFSIZE); // 读取字库
    for (int i = 0; i < BUFFSIZE; i++)
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

void die(uint16_t n)
{
    char buf[100];
    buf[0] = (char)n; // 让编译器没法偷懒把这层纸删掉
    if (n > 0)
        die(n - 1); // 喊 n 次就一层层回头
}

void add_finger(void)
{
    mg200_add_finger();
    while (1)
    {
        mg200_compare();
    }
}

/* ==================== 字库 flash 全量回读(定位完即删) ==================== */
#define FONT_DUMP_LEN 0x1D3800u /* 略大于 font_all.bin(0x1D3374),够覆盖整片字库 */

/**
 * @brief  把 W25Q 里的字库原样吐到 USART1,PC 端存盘后与源文件逐字节对比
 * @param  无
 * @return 无
 * @note   不经过 printf,直接写 DR,省掉格式化开销;波特率 115200 时约 166 秒。
 *         PC 端先找到 ASCII 同步头 "===DUMP START===",再往后收 FONT_DUMP_LEN 字节。
 */
void font_dump(void)
{
    static uint8_t dumpbuf[512]; /* 放静态区,避开只有 1KB 的主栈 */
    uint32_t addr;
    uint16_t i;

    printf("===DUMP START===\r\n");

    for (addr = 0; addr < FONT_DUMP_LEN; addr += sizeof(dumpbuf))
    {
        W25Qxx_Read_Bytes(addr, dumpbuf, sizeof(dumpbuf));
        for (i = 0; i < sizeof(dumpbuf); i++)
        {
            while ((USART1->SR & (1 << 7)) == 0)
                ; /* 等 TXE 置位,与 fputc 同理,只是绕过 stdio */
            USART1->DR = dumpbuf[i];
        }
    }

    printf("\r\n===DUMP END===\r\n");
}

void nv400f_test(void)
{
    static const u8 voice_test_addr[3] = {0x00, 0x01, 0x02};
    u8 k = 0;

    while (1)
    {
        printf("语音:发送地址 0x%02X\r\n", voice_test_addr[k]);
        NV400F_SendCmd(voice_test_addr[k]);

        k++;
        if (k >= 3)
        {
            k = 0;
        }

        NV400F_DelayUs(2000000); // 命令间隔 2s,留出耳朵分辨的时间(手册只要求 >50ms)
    }
}
