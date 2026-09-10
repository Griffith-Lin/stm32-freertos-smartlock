# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概览

STM32F401xx(Cortex-M4 @84MHz)固件:ST 标准外设库(StdPeriph,**不是** HAL)+ FreeRTOS。裸机初始化外设 → 阻塞式指纹录入 → 启动调度器跑按键等业务任务。
工具链:Keil **AC5**(armcc),C99,microlib,优化等级 0,`all-warnings`(抑制 #1、#1295)。

## 构建 / 烧录 / 调试

构建链是 **eIDE**(VSCode 扩展 `cl.eide-3.27.2`),工程描述在 `.eide/eide.yml`;根目录 `project.uvprojx` 是历史遗留(已落后:不含 MG200.c/mcu_wake.c/learn.c 等新文件),不要用它构建,一律以 eIDE 为准。

- VSCode 任务(`.vscode/tasks.json`):`build` / `rebuild` / `clean` / `flash` / `build and flash`。
- 命令行等价构建(已验证可用,在仓库根目录的 Git Bash 里跑):

  ```bash
  "$HOME/.vscode/extensions/cl.eide-3.27.2/res/tools/win32/unify_builder/unify_builder.exe" \
    -p "build/Target 1/builder.params"
  ```

  目标名带空格(`Target 1`),路径必须加引号。
- 产物:`build/Target 1/project.elf|.hex|.s19`,目标文件在 `build/Target 1/.obj/`(可按时间戳判断某文件是否真的重新编译过)。
- **新增 .c/.h 文件必须经 eIDE 加入工程**(写进 `.eide/eide.yml` 的 `virtualFolder`),否则不参与编译,头文件也搜不到。
- 烧录:OpenOCD(cmsis-dap,stm32f4x,base 0x08000000);调试器 cortex-debug,配置来自 eIDE 用户设置(`.vscode/launch.json` 为空是正常的)。
- 没有单元测试/lint;验证方式 = 编译通过 + 上板看 USART1 打印。

## 代码结构

- [user/main.c](user/main.c) 是唯一入口,初始化顺序有依赖,不要随手调整:`NVIC_PriorityGroup_4` → `mcu_wkup_Config()`(必须最先)→ `TIM10_Task_Config`(1ms 时基)→ `usart1_Config(115200)` → led/L9110S/SPI+W25Q/LCD → `mg200_Config` → `mg200_add_finger()`(阻塞轮询,采满 3 枚指纹才返回;**它不返回,后面代码都不会跑**)→ `mcu_was_standby_wake()` 分流 → `key_tasks_init()` → `vTaskStartScheduler()`。
- `printf` 重定向在 main.c 的 `fputc` → USART1(PA9/PA10,115200),是唯一的调试输出通道。
- [api/](api/) = 一外设一模块的 .c/.h;`lib/` = StdPeriph 源码;`freertos/` = 内核 + port + `FreeRTOSConfig.h`;`.cmsis/`、`RTE/`、`startup/` 为标准件。
- [user/learn.c](user/learn.c) = FreeRTOS 学习演示(任务/队列/信号量/互斥量),统一由 `learn_tasks_init()` 创建,main.c 里默认注释掉。业务代码不要依赖它。
- `docs/`、`学习笔记/` 是个人笔记,不参与编译(注意 `docs/FreeRTOS移植笔记.md` 内容其实是大创申报,与代码无关)。

## 硬件与引脚

| 引脚 | 用途 |
| --- | --- |
| PA0 | WKUP,STANDBY 唤醒专用(上升沿) |
| PA4 / PA5/6/7 | SPI1 CS / SCK/MISO/MOSI(W25Q Flash) |
| PA9 / PA10 | USART1 TX/RX,printf 控制台 |
| PB1、PB10、PB12~15 | LCD |
| PB3 / PB4 | L9110S 电机 FOR/BAK |
| PB8 / PB9 | LED 红/绿(低电平亮) |
| PC13 | 按键(key.c 状态机) |
| PC0 | MG200 DETECT(有手指=高电平) |
| PC1 | MG200 电源使能(拉高后模块约 1~2s 才应答) |
| PC6 / PC7 | USART6 TX/RX(AF8),接 MG200 指纹模块 |

## FreeRTOS 与时间

- 两个 1ms 时基:FreeRTOS tick(`configTICK_RATE_HZ = 1000`)和 TIM10(`tim.h` 的 `GetTim10Tick()`/`TIM10_delay()`)。TIM10 不依赖调度器,裸机阶段(main 里的 MG200 轮询)也用它计时。`delay.c` 的 `Delay_Ms/Delay_Us` 是 NOP 忙等,只在裸机阶段用,别在任务里长时间调。
- `configMAX_PRIORITIES = 5`(优先级 0~4);任务栈深度单位是**字**(4 字节);堆只有 8KB(heap_4),加任务前先算余量,否则会打到 `vApplicationMallocFailedHook` 死循环。
- 中断优先级分组必须是 `NVIC_PriorityGroup_4`(main 开头已做);`configMAX_SYSCALL_INTERRUPT_PRIORITY = 0xB0`,ISR 里只能用 `...FromISR` API。
- `FreeRTOSConfig.h` 里未打开的 `INCLUDE_xxx` API 会在链接时报 `Undefined symbol`,要用先开配置。
- 当前任务:`key_tasks_init()` 建 `key_scan`(10ms 轮询 GPIO 状态机消抖)通过 `key_queue` 把事件发给 `key_action`;长短按阈值 600ms。任务/队列/栈的清单见 [api/key.c](api/key.c)。

## 关键外设约定

- **mcu_wake**:PA0 唤醒 STANDBY = 整片复位,一切从零重跑;开机用 `mcu_was_standby_wake()`(顺带清标志)区分"上电复位"与"待机唤醒"。本模块完全工作在 FreeRTOS 之外。
- **MG200 指纹(USART6)**:帧 = `6C | 发送地址 | 接收地址 | 命令/应答码 | 数据… | 校验和`;校验和 = 发送地址~最后一个数据字节之和的低 8 位。应答数据区长度**随命令变**(0x51/0x54/0x7F = 3 字节,0x58 = 5 字节),查 `mg200_data_len()`。接收必须先同步帧头 `0x6C` 且每字节带超时:上电毛刺会在 RX 里留一个杂字节,若按定长盲读,整帧永久右移一字节且错位不自愈(历史 bug,别退回那种写法)。
- USART6 的轮询接收和中断接收**不要混用**:ISR 会先读走 RXNE,轮询就永远等不到数据。

## 代码约定

- 注释、打印提示一律中文;函数头用 doxygen 风格写清参数/返回/注意事项(现有代码风格,新代码照做)。
- 用户是嵌入式学习者:解释要说清"为什么",改动硬件驱动后至少编译一次验证。

## 已知工具链坑

- eIDE 调试概率报"5 秒内无法启动 GDB"与本仓库代码无关:cortex-debug 的 gdb 启动超时硬编码 5s,eIDE 默认 GCC 15.2 自带的 gdb 冷启动要 7.3s。已在用户级 `settings.json` 把 `cortex-debug.gdbPath`、`EIDE.ARM.GCC.InstallDirectory`、`cortex-debug.armToolchainPath` 三处指到 10.3(`gcc_arm_v10`),别改回 15.2。本工程用 AC5 编译,这三处不影响构建。
