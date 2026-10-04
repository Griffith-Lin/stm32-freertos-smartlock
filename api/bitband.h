#ifndef __BITBAND_H_
#define __BITBAND_H_

/*
bitband.h 是你工程里的位带操作宏定义文件。

STM32F4 的 SRAM/外设区每个 bit 在内存里都有一个独立的 32 位别名地址，
写 1 就是置位，写 0 就是清零。bitband.h 把这些包装成了简洁的宏：
*/

//计算别名区的地址：
//addr -- 存储器的地址    bit --要操作的位
#define BITBAND(addr,bit) (((addr&0xF0000000)+0x2000000)+ ((addr&0xFFFFF)*8+bit)*4)
                                   //别名区基地址         别名区的偏移量

/* volatile 不能省。
 * 它是在告诉编译器:"这块内存会自己变,别优化我。"
 * 少了它,编译器有权认为下面这种写法没有可观察的副作用,直接删掉前两次写:
 *     PBout(8) = 1;
 *     PBout(8) = 0;
 *     PBout(8) = 1;
 * 实测:同一段代码 -O0 下三条 STR 都在,-O2 下只剩一条 —— 而编译器一句警告都不给。
 * 现象是低优化时一切正常,一开优化时序类代码(软件 SPI、单总线)就随机乱掉。
 * 现在工程是 optimization: level-0 所以看不出问题,但雷不能埋着。 */
#define MEMADDR(addr,bit)  *(volatile unsigned int *)BITBAND(addr,bit)


//IDR
#define PAin(bit)  MEMADDR((unsigned int)&GPIOA->IDR,bit)
#define PBin(bit)  MEMADDR((unsigned int)&GPIOB->IDR,bit)
#define PCin(bit)  MEMADDR((unsigned int)&GPIOC->IDR,bit)
#define PDin(bit)  MEMADDR((unsigned int)&GPIOD->IDR,bit)
#define PEin(bit)  MEMADDR((unsigned int)&GPIOE->IDR,bit)
#define PFin(bit)  MEMADDR((unsigned int)&GPIOF->IDR,bit)
#define PGin(bit)  MEMADDR((unsigned int)&GPIOG->IDR,bit)
#define PHin(bit)  MEMADDR((unsigned int)&GPIOH->IDR,bit)

//ODR
#define PAout(bit)  MEMADDR((unsigned int)&GPIOA->ODR,bit)
#define PBout(bit)  MEMADDR((unsigned int)&GPIOB->ODR,bit)
#define PCout(bit)  MEMADDR((unsigned int)&GPIOC->ODR,bit)
#define PDout(bit)  MEMADDR((unsigned int)&GPIOD->ODR,bit)
#define PEout(bit)  MEMADDR((unsigned int)&GPIOE->ODR,bit)
#define PFout(bit)  MEMADDR((unsigned int)&GPIOF->ODR,bit)
#define PGout(bit)  MEMADDR((unsigned int)&GPIOG->ODR,bit)
#define PHout(bit)  MEMADDR((unsigned int)&GPIOH->ODR,bit)




#endif

