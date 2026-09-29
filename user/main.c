#include "main.h"
#include "app_config.h" // 组件裁剪开关 APP_USE_xxx:main() 里每个组件都被 #if 包着

// 下面按"main() 里真正调到了谁"逐个列出,用不到的不加。
// (原来靠 main.h 那个万能头一次性带进来 20 个外设头文件,连 w25qxx.h 都带了
//  两遍,而 main() 根本没直接调过 W25Qxx_*;说明见 main.h 顶部)
#include "led.h"      // led_Config / LED_BLUE_toggle
#include "usart1.h"   // usart1_Config(printf 的物理出口)
#include "tim.h"      // TIM10_Task_Config(1ms 时基,裸机阶段也靠它)
#include "L9110S.h"   // L9110S_Config(开锁电机)
#include "spi.h"      // spi1_w25_Config(W25Q:字库存里面)
#include "lcd.h"      // LCD_Init
#include "i2c.h"      // i2c_bus_config(CY9CM 和 AT24C04 两条总线)
#include "CY9CM.h"    // CY9CM_Init / cy9cm_tasks_init
#include "lock.h"     // lock_init / lock_tasks_init
#include "MG200.h"    // mg200_Config / mg200_get_figer_num
#include "finger.h"   // finger_tasks_init
#include "NV400.h"    // NV400F_Init / NV400F_DelayUs
#include "wifi.h"     // wb2_Config / wifi_tasks_init
#include "key.h"      // key_tasks_init
#include "mcu_wake.h" // PA0 待机唤醒(业务)
#include "learn.h"    // 学习演示任务(learn.c):互斥量/计数信号量/LCD测试等,与业务无关

#include "FreeRTOS.h"
#include "task.h"

// pa9 u1_tx  pa10 u1_rx

// 🔴 L0 生死线代码 —— 直接轮询 USART1->SR 的 TXE 位再写 DR:位号或判断方向写反就是死等/丢字符,而且它是全工程唯一的 printf 出口,坏了所有调试手段跟着哑
int fputc(int c, FILE *stream)
{
    while ((USART1->SR & (1 << 7)) == 0)
        ; // 当发送数据缓冲区不为空时，循环发送,直到发送完毕
    USART1->DR = c;
    return c;
}

// 这两个 Hook 是 FreeRTOS 的错误处理机制：发生致命错误时，如果直接返回，任务调度还会继续用坏掉的堆/栈运行，导致更隐蔽的问题，所以惯例就是就地死循环"停住"，并配合 printf/断点排查。
/**
 * 🔴 L0 生死线代码 —— 堆耗尽后的最后一道闸:这里一旦返回,后续代码会拿着分配失败的空指针继续跑,所以就地死循环等 SWD 断点
 * @brief FreeRTOS 堆内存分配失败回调(由 configUSE_MALLOC_FAILED_HOOK 使能)
 * @return 无
 * @note 分配失败多为 configTOTAL_HEAP_SIZE 过小;卡死便于断点排查
 */
void vApplicationMallocFailedHook(void)
{
    for (;;) // 卡死在这，区分于while(1)——后者是任务主循环，调用 vTaskDelay(500) 主动让出 CPU，不是卡死，而是 FreeRTOS 任务的标准写法。
        ;    /* 堆内存分配失败（configTOTAL_HEAP_SIZE 不够） */
}

/*
@brief 任务栈溢出钩子函数
FreeRTOS 任务栈溢出时会调用这个函数，参数：

TaskHandle_t xTask —— 出事任务的"身份证指针"，指向内核的 TCB（任务控制块）,xTaskCreate 最后一个参数 pxCreatedTask 让你拿回这个指针；你当时传了 NULL 表示"我不要"，但 FreeRTOS 内部自己一直存着
任务出事后，FreeRTOS 把出事任务自己的户口本地址塞给 Hook 的第一个参数，这样你就能知道是哪个任务出事了

char *pcTaskName —— 出事任务的"名字"

FreeRTOS 要求 Hook 里要么 for(;;) 停住，要么删掉肇事任务——因为它的栈已经不可信了，让它继续跑只会更乱。
 */
/**
 * 🔴 L0 生死线代码 —— 栈已经越界、现场不可信:先打印肇事任务名再 vTaskDelete 停住,删错对象或顺序反了会把内核数据结构踩得更烂
 * @brief FreeRTOS 任务栈溢出回调(由 configCHECK_FOR_STACK_OVERFLOW 使能)
 * @param xTask 溢出任务的句柄(TCB 指针)
 * @param pcTaskName 溢出任务的名字
 * @return 无
 * @note 任务栈已不可信,打印现场后删除肇事任务并停住
 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    printf("stack overflow %s\r\n", pcTaskName);
    // printf("优先级 = %d\r\n", uxTaskPriorityGet(xTask));  // 查它的优先级
    vTaskDelete(xTask); // 或者：把肇事任务直接"枪毙"，让别的任务继续跑
    for (;;)
        ; /* 任务栈溢出（栈配小了或递归太深） */
}

// 🟢 L2 工具代码 —— 函数体是空的,只留了一行注释掉的 LED 可视化调用,纯展示挂点;往这里加东西时记住它跑在空闲任务里、绝不能阻塞
void vApplicationIdleHook(void) // FreeRTOS 空闲任务钩子函数,由 configUSE_IDLE_HOOK 使能
{
    // LED_BLUE_toggle();空闲状态可视化
}



/**
 * 🔴 L0 生死线代码 —— 初始化顺序是硬依赖链(mcu_wkup_Config 必须最先、lock_init 必须赶在调度器前),顺序错了待机唤醒认不出、EEPROM 忙等会饿死任务
 * @brief 主函数:外设初始化后创建任务并启动 FreeRTOS 调度器
 * @return 无
 * @note 正常情况下 vTaskStartScheduler 不会返回
 */
int main()
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    mcu_wkup_Config(); // 开 PWR 时钟 + 配 PA0(WKUP),须最先于其他外设初始化

    // ===== 基础设施(不参与裁剪的地基)=====
    // 这三样不属于任何"组件",而是所有组件的前提:
    //   TIM10   —— 全局 1ms 时基,连裸机阶段(MG200 轮询录入)都在用它计时
    //   USART1  —— printf 的物理出口,关掉等于把眼睛蒙上
    //   LED     —— 状态指示灯,好几个组件都拿它当"我还活着"的信号
    TIM10_Task_Config(84, 1000); // 1ms 毫秒时基(TIM10 中断维护 tim10_tick_ms)
    usart1_Config(115200);
    led_Config();

    // ===== 按 APP_USE_xxx 裁剪的外设初始化(开关在 user/app_config.h)=====
    // ⚠ 这些 #if 块之间的**顺序不能动**,它是硬依赖链:
    //      spi1_w25_Config 必须在 LCD_Init 之前 —— 屏的字库就存在 W25Q 里
    //      i2c_bus_config  必须在 CY9CM_Init / lock_init 之前 —— 先配总线再用
    //    全部开关打开时,展开出来的顺序与原版逐行一致。
#if APP_USE_LOCK
    L9110S_Config();                  // 开锁电机
#endif

#if APP_USE_LCD
    spi1_w25_Config();                // W25Q Flash:屏的字库存里面
    LCD_Init();                       // 内部顺带创建 LCD 互斥量(见 lcd.c 顶部说明)
#endif

#if APP_USE_FINGER
    mg200_Config(115200);             // MG200 指纹:PC1 上电 + USART6
#endif

#if APP_USE_WIFI
    wb2_Config();                     // WB2 联网模块:USART2(PA2/PA3),只配硬件不发命令
#endif

#if APP_USE_TOUCH
    i2c_bus_config(&i2c_bus_cy9cm);   // CY8CMBR3116 按键:SCL=PB6 SDA=PB7
#endif

#if APP_USE_LOCK
    i2c_bus_config(&i2c_bus_at24c04); // AT24C04A EEPROM:SCL=PA8 SDA=PC9(存密码用)
#endif

#if APP_USE_TOUCH
    CY9CM_Init();                     // RES 复位脉冲 + (必要时)写 128 字节配置
#endif

#if APP_USE_LOCK
    // 密码锁:从 EEPROM 载入密码(首次上电写默认值),然后画出输入界面。
    // 必须在调度器起来之前调用 —— 首次上电会走一次 EEPROM 写,而那是忙等。
    lock_init();
#endif
   
#if APP_USE_VOICE
    NV400F_Init();
    NV400F_DelayUs(300000); // 等模块上电初始化:手册要求 >200ms,这里给 300ms
#endif

    
    // ===== 待机唤醒识别(复位原因分流)=====
    // STANDBY 唤醒 = 整片复位,一切归零重跑;靠 WUF 标志认亲,分流出不同启动流程
    if (mcu_was_standby_wake()) // 长按 PC13 进待机 → 按 PA0 唤醒后走这里
    {
        printf("wake from STANDBY!\r\n"); // 冷启动不会打印这一行
        LED_BLUE_toggle();                // 亮蓝灯标记"我从待机醒来"(冷启动保持灭)
    }

    
#if APP_USE_MG200_DUMP
    mg200_get_figer_num(); // 开机打印指纹模块里已录的指纹数量
#endif


    // ===== 业务任务 =====
    // 任务栈是 xTaskCreate 时从 FreeRTOS 那 12KB 堆里切走的,建了就永久占着
    // (跟它干不干活无关)。全开时约 1728 字 ≈ 6.9KB,占掉堆的一多半 ——
    // 这就是裁剪开关真正省下来的东西(不是 Flash,Flash 由链接器自动回收)。
#if APP_USE_KEY
    key_tasks_init(); // 按键:建事件队列 + 创建 key_scan(生产)/ key_action(消费) 两个任务
#endif
#if APP_USE_TOUCH
    cy9cm_tasks_init(); // 触摸板:建按键事件队列 + 创建扫描任务
#endif
#if APP_USE_LOCK
    lock_tasks_init();  // 密码锁:消费触摸板按键,校验密码后驱动电机开锁
#endif
#if APP_USE_FINGER
    finger_tasks_init(); // 指纹锁:轮询 MG200 的 DETECT 脚,比对通过后走同一个开锁流程
#endif
#if APP_USE_WIFI
    wifi_tasks_init();   // 联网:WB2 模块跑 AT 命令连 WiFi→MQTT,优先级最低(1)
#endif

    // ===== 学习演示任务(与业务无关) =====
#if APP_USE_LEARN
    learn_tasks_init();
#endif

    vTaskStartScheduler(); // 启动调度器,正常情况下不返回

    printf("sched returned!\r\n"); // 调度器返回了，启动失败
    for (;;)
        ;
}
