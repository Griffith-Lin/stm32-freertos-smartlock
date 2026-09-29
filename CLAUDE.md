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
- [api/admin.c](api/admin.c) + [api/admin.h](api/admin.h) = 管理员模式:用密码开锁后的 15 秒窗口里输入 `111#`/`222#`/`333#`/`444#`/`555#` 进子模式。**它自己不建任务**,跑在 `lock_task` 里(见下面「管理员模式」一节)。
- [user/learn.c](user/learn.c) = FreeRTOS 学习演示(任务/队列/信号量/互斥量),统一由 `learn_tasks_init()` 创建,main.c 里默认注释掉。业务代码不要依赖它。
- `docs/`、`学习笔记/` 是个人笔记,不参与编译(注意 `docs/FreeRTOS移植笔记.md` 内容其实是大创申报,与代码无关)。

## 硬件与引脚

| 引脚 | 用途 |
| --- | --- |
| PA0 | WKUP,STANDBY 唤醒专用(上升沿) |
| PA4 / PA5/6/7 | SPI1 CS / SCK/MISO/MOSI(W25Q Flash) |
| PA9 / PA10 | USART1 TX/RX,printf 控制台 |
| PA15 | RC522 片选 SDA。**兼 JTDI**:STM32F4 无 SWJ_CFG 寄存器,配成 GPIO 输出即抢占成功(PB3/PB4 同理);代价是 JTAG 不可用,SWD 不受影响 |
| PB1、PB10、PB12~15 | LCD |
| PB3 / PB4 | L9110S 电机 FOR/BAK |
| PB8 / PB9 | LED 红/绿(低电平亮) |
| PC13 | 按键(key.c 状态机) |
| PC0 | MG200 DETECT(有手指=高电平) |
| PC1 | MG200 电源使能(拉高后模块约 1~2s 才应答) |
| PC6 / PC7 | USART6 TX/RX(AF8),接 MG200 指纹模块 |
| PC10 / PC11 / PC12 | SPI3 SCK/MISO/MOSI(AF6),接 RC522 刷卡模块 |
| PD2 | RC522 复位 RST(低电平有效) |

## FreeRTOS 与时间

- 两个 1ms 时基:FreeRTOS tick(`configTICK_RATE_HZ = 1000`)和 TIM10(`tim.h` 的 `GetTim10Tick()`/`TIM10_delay()`)。TIM10 不依赖调度器,裸机阶段(main 里的 MG200 轮询)也用它计时。`delay.c` 的 `Delay_Ms/Delay_Us` 是 NOP 忙等,只在裸机阶段用,别在任务里长时间调。
- `configMAX_PRIORITIES = 5`(优先级 0~4);任务栈深度单位是**字**(4 字节);堆 12KB(`configTOTAL_HEAP_SIZE`,heap_4),**加任务前先算余量**,否则会打到 `vApplicationMallocFailedHook` 死循环。
  已切出去的任务栈:`wifi` 512 + `lock` 384 + `finger`/`key_scan`/`cy9cm_scan`/`rc522` 各 256 + `key_action` 128,再加 idle 和 timer 各 128(字)= **2304 字 = 9216 字节**;连同 9 个 TCB(每个约 110 字节)和 3 个队列,合计约 10.5KB,**只剩约 1.6KB**。`rc522_tasks_init()` 里打了一行 `xPortGetFreeHeapSize()` 实测值,加东西前后对比那行数字即可。
  (`lock` 从 320 加到 384 是管理员模式带来的:调用链多了一层 `lock_handle_key → admin_handle_key → lock_lcd_big_line`,而栈溢出会被 `vApplicationStackOverflowHook` 变成"删任务 + 死循环",上板表现只是"按了没反应",极难排查。)
- 中断优先级分组必须是 `NVIC_PriorityGroup_4`(main 开头已做);`configMAX_SYSCALL_INTERRUPT_PRIORITY = 0xB0`,ISR 里只能用 `...FromISR` API。
- `FreeRTOSConfig.h` 里未打开的 `INCLUDE_xxx` API 会在链接时报 `Undefined symbol`,要用先开配置。
- 当前任务:`key_tasks_init()` 建 `key_scan`(10ms 轮询 GPIO 状态机消抖)通过 `key_queue` 把事件发给 `key_action`;长短按阈值 600ms。任务/队列/栈的清单见 [api/key.c](api/key.c)。

## 关键外设约定

- **mcu_wake**:PA0 唤醒 STANDBY = 整片复位,一切从零重跑;开机用 `mcu_was_standby_wake()`(顺带清标志)区分"上电复位"与"待机唤醒"。本模块完全工作在 FreeRTOS 之外。
- **MG200 指纹(USART6)**:帧 = `6C | 发送地址 | 接收地址 | 命令/应答码 | 数据… | 校验和`;校验和 = 发送地址~最后一个数据字节之和的低 8 位。应答数据区长度**随命令变**(0x51/0x54/0x7F = 3 字节,0x58 = 5 字节),查 `mg200_data_len()`。接收必须先同步帧头 `0x6C` 且每字节带超时:上电毛刺会在 RX 里留一个杂字节,若按定长盲读,整帧永久右移一字节且错位不自愈(历史 bug,别退回那种写法)。
- USART6 的轮询接收和中断接收**不要混用**:ISR 会先读走 RXNE,轮询就永远等不到数据。
- **AT24C04 EEPROM 有三个运行时写入者,而 bit-bang I2C 没有任何互斥**:wifi 配网(`wifi_ble_provision` → `wifi_cfg_save`)、刷卡录入/删除(`rc522_enroll`/`rc522_erase`)、改密码(`lock_set_password`)。同一条总线被两个任务同时搓,事务会串。**这是既有隐患**(wifi + rc522 本来就并存),加管理员模式又添了第三处。再加写入者之前先考虑给 I2C 加互斥量(`api/i2c.h` 里目前一个 Lock/Mutex 都没有)。
- EEPROM 布局(512 字节):0–7 锁密码(标志 0xA5)、8–105 WiFi 凭据(标志 0xA6 在 105)、112–117 授权卡号(标志 0xA7)。选 112 是因为 AT24C04 页大小 16 且页写不进位,**整块必须落在同一页才能一次原子写**。注意 `AT24C04_page_write()` 末尾有 `Delay_Ms(10)`,而 `delay.c` 按 168MHz 标定、本片跑 84MHz → 实际约 **20ms 忙等**。

## 管理员模式(api/admin.c)

电容板用密码开锁成功后进 15 秒窗口,期间输入 `111#`/`222#`/`333#`/`444#`/`555#` 进子模式:**111#** 清空指纹库 + 删授权卡、**222#** 改密码(输两遍)、**333#** 加 RFID 卡、**444#** 加指纹、**555#** 蓝牙配网(20 秒,成了就存 EEPROM 换新网,不成继续用当前网络)。任一子模式做完就退出。

- **它没有自己的任务**,整台状态机跑在 `lock_task` 里(入口 `admin_tick()` / `admin_handle_key()`,在 [api/lock.c](api/lock.c) 的两个钩子点被调用)。原因是它必须在"锁还在开锁/保持/回锁"的同时保持响应 —— 一旦阻塞等手指,`lock_tick()` 就停摆、锁不回锁。
- **长活派给外设所有者**:admin 只 `xxx_request_yyy()` 投个标志,真正的动作由 `finger_task`(MG200+DETECT 脚)/`rc522_task`(RC522)/`wifi_task`(USART2+AT 状态机)干。标志的"取出+清零"必须在同一临界区(见 `finger_take_request()`)。222# 只改 EEPROM,admin 自己做。
- ⚠ **派活和退出的次序是"先 `admin_exit()` 再置请求标志"**,别写反:`admin_exit()` → `lock_screen_give()` 里含一次约 50ms 的擦屏,反过来的话被派活的任务有约 50ms 窗口正好看到屏还占着,于是"活照干、屏不画"——用户按了 555# 却全程黑屏。
- **15 秒是"按键重置"的倒计时**,不是固定窗口:`ADMIN_WINDOW_MS` 在 [api/admin.h](api/admin.h)。因为 `111#` 要按 4 下,固定窗口会让手慢的人按到一半被踢出去。子模式内另有 `ADMIN_SUB_TIMEOUT_MS`(20s)兜"按一半走人"。
- ⚠ **画屏也是"阻塞",和"等手指"一样致命** —— 这是踩过的坑。整屏菜单 6 行 × 5760 像素,底层 `LCD_Set_Point` 是**一个像素开一次 ST7789 地址窗口**,一行一两百毫秒、整屏一秒往上;而 `lock_task` 同一时刻只能干一件事,画屏期间就回不到 `lock_tick()`。原来的现象:密码开锁成功那一刻的调用链是 `lock_verify → lock_open`(电机已正转)`→ admin_begin()`,而 `admin_begin()` 同步画整屏菜单,于是负责刹车的 `lock_tick()` 整整一秒没机会跑 —— **开锁时间远远超过 `LOCK_MOTOR_OPEN_MS`,锁舌顶到头还在堵转**。
  规矩是三条,缺一不可:① 按键路径(`admin_begin`/`admin_handle_key`/`admin_submit`)一个像素都不许碰,只改状态 + `admin_set_redraw()` 登记;② `admin_flush_redraw()` 落笔前先问 `lock_motor_running()`,电机通着电(只有 `LOCK_OPENING`/`LOCK_CLOSING` 这 700ms 通电,保持期内是刹车的)就继续欠着;③ 整屏重画**一次只画一行**,让 `lock_task` 每两行之间都能回去收按键和远程命令。
- `lock_task` 里 **`lock_tick()` 必须排在 `admin_tick()` 前面**,而且每拍都推(不能只在"两个队列都空"那一拍推)。刹车是全系统最不能等的一件事,排在画屏后面就等着超时。
- **LCD 版面**(全部集中在 [api/lock.c](api/lock.c) 开头那一小段宏里,别的模块只用相对行号 0~5,所以改那里就够了):标题行、大提示区、密码输入行三者是**一组对好的数**,已经上移 30 像素 —— 标题 50~81、大提示区 82~225、输入行 130~161。要整体上下移**只改 `LOCK_UI_UP` 一个宏**,别去单独改某一个:`LOCK_LCD_TITLE_ROW`/`LOCK_LCD_INPUT_ROW`/`LOCK_BIG_ROW` 都从它减去。三条不能破的相对关系:① 大提示区从标题行下沿 +1 开始,**一个像素都不碰标题行**,于是 `lock_tick()` 刷标题和提示区写字永远不会互盖,不需要引入"屏幕所有权协议";② 密码输入行整个落在大提示区第 2、3 行里,所以进管理员模式不用整屏清一次就能盖住它;③ 大提示区 6×24=144 行,baseline 底边 256 离屏底 320 还有余量。一行 24 像素 × 20 个 ASCII 字符(20×12=240=屏宽),每行必须写满 20 个字符(不足补空格)否则右侧留残影;文字一律 **ASCII**。
- **屏幕占用位 `LOCK_SCREEN_*`**:四方(admin/指纹/刷卡/配网)各占一位,`lock_screen_take()` 是**原子检查+置位**,拿不到就"活照干、别画界面" —— 111# 会同时给指纹和刷卡派活,两个都画就会互相盖。用独立位而非计数器,是因为四方的 `++` 读-改-写会被任务切换打断而丢更新。

### 555# 手动蓝牙配网

[api/wifi.c](api/wifi.c) 的 `wifi_ble_provision(window_ms, admin_ui)` 有两个调用点,靠 `admin_ui` 分叉:

| | 开机自动那次 | 555# 手动那次 |
| --- | --- | --- |
| 窗口 | `WIFI_T_PROV_MS` 3 分钟 | `WIFI_PROV_ADMIN_MS` 20 秒 |
| 触发 | `s_prov_tried` 拦着,只做一次 | 每次按键都做 |
| 画面 | 改标题行(`lock_lcd_set_title`) | 占大提示区(`lock_screen_take(LOCK_SCREEN_WIFI)`) |

- **画面为什么必须分两套**:开机那次锁一定空闲,写标题行没人抢;555# 是在 `lock_task` 还在跑 tick 的时候进来的,再写标题行就是两个任务同时写软件 SPI 的 LCD。原来那段注释里写着"以后加了按键手动配网这个前提立刻就没了",这就是那一刻。
- **`s_prov_req` 是所有 AT 等待的打断开关**。管理员按 555# 时 `wifi_task` 很可能正卡在超时 25 秒的 `AT+WJAP` 里,不打断的话 20 秒窗口一半就没了。打断点有三处:`wifi_get_byte`(等待循环)、`wifi_at_ex`、`wifi_wait_event`(后两处是防前者返回 0 后空转烧 CPU 到 timeout)。打断会留下没人收的残应答,所以受理前必须 `wifi_prov_flush_rx()` 倒一次。
- **`s_prov_run` 是配网的闸门**:配网期间 `wifi_request_provision()` 拒绝受理新请求——否则收尾那条 `AT+WCONFIG=0` 会被自己人截断,模块一直停在配网模式,等于长期开着"旁边谁都能把锁改到别的 WiFi"的口子。置位/清位包在 `wifi_ble_provision_run()` 里,免得漏掉某个 return 分支。
- **`wifi_delay()` 代替了重试环里的 `vTaskDelay(10s)`**:一次睡满 10 秒的话,窗口里按的 555# 要等睡醒才被看见。凡是 `wifi_task` 里超过几十毫秒的等待都得走它。
- ⚠ **20 秒很紧**:用户得掏手机、开 ESP BluFi App、连模块的 BLE 广播、填 SSID 和密码。嫌短改 `WIFI_PROV_ADMIN_MS` 一个宏即可。窗口关了之后还有 `WIFI_PROV_SETTLE_MS`(8 秒)判模块自己连上没有,所以最坏 28 秒收工。

## 代码约定

- 注释、打印提示一律中文;函数头用 doxygen 风格写清参数/返回/注意事项(现有代码风格,新代码照做)。
- 用户是嵌入式学习者:解释要说清"为什么",改动硬件驱动后至少编译一次验证。

## 已知工具链坑

- eIDE 调试概率报"5 秒内无法启动 GDB"与本仓库代码无关:cortex-debug 的 gdb 启动超时硬编码 5s,eIDE 默认 GCC 15.2 自带的 gdb 冷启动要 7.3s。已在用户级 `settings.json` 把 `cortex-debug.gdbPath`、`EIDE.ARM.GCC.InstallDirectory`、`cortex-debug.armToolchainPath` 三处指到 10.3(`gcc_arm_v10`),别改回 15.2。本工程用 AC5 编译,这三处不影响构建。
