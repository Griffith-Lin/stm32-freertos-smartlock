#include "main.h"


//pa9 u1_tx  pa10 u1_rx
//cp2102连在串口1上
int fputc(int c, FILE *stream)
{
    while ((USART1->SR & (1 << 7)) == 0)
        ; // 当发送数据缓冲区不为空时，循环发送,直到发送完毕
    USART1->DR = c;
    return c;
}



int main()
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_3);

    TIM10_Task_Init(84,1000);
    usart1_ini(115200);
    led_ini();
    L9110S_ini();
    spi1_w25_ini();
    key_ini();

    
    while(1)
    {

        
    }
}


