#ifndef __MAIN_
#define __MAIN_

// ══════════════════════════════════════════════════════════════════════════
//  main.h 只放"全工程都成立的平台前提",不再放任何具体外设。
//
//  【它原来是什么样,以及为什么必须改】
//      这里 include 了 20 个 api 头文件 —— led / usart1 / L9110S / delay /
//      tim / w25qxx(还写了两遍)/ spi / key / dma / lcd / debug / MG200 /
//      CY9CM / lock / finger / NV400 / wifi。
//      后果非常具体:learn.c 只想用 LCD_Clear 画个方块,却被迫把 WiFi 的 AT
//      命令、指纹的帧解析、密码锁状态机一起拖进来编译。改 wifi.h 里一个宏,
//      learn.c 要跟着重编 —— 这就是"高耦合"在你工程里的实际样子。
//      (顺带一提,stm32f4xx_it.c 也 include 了它,而那个文件一个外部符号都
//       没用 —— 6 个异常处理全是 while(1)。纯属白加。)
//
//  【现在的规矩:谁用谁 include】
//      main.c          → 自己列它在 main() 里真正调到的那些外设(见 main.c 顶部)
//      learn.c         → 只 include lcd.h / led.h
//      stm32f4xx_it.c  → 只留 stm32f4xx_it.h
//
//  想加组件,请加到**用它的那个 .c** 里,别加回这里。
//  这个文件保留下来,是因为它是 eIDE/Keil 工程里"主头文件"的惯例位置,
//  放平台级前提正合适,而且 main.c 第一行就是它,不改成空文件更省事。
// ══════════════════════════════════════════════════════════════════════════

#include "stm32f4xx.h"   // CMSIS + 标准外设库(stm32f4xx_conf.h 由它自动带出)
#include "stdio.h"       // printf;物理出口是 main.c 的 fputc → USART1

#endif
