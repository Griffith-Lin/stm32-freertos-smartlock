#ifndef __APP_CONFIG_
#define __APP_CONFIG_

// ══════════════════════════════════════════════════════════════════════════
//  组件裁剪开关(四大设计原则之"可配置与可裁剪")
//
//  1 = 启用   0 = 裁掉
//
//  为什么需要这个文件,而不是在 main.c 里注释掉一行?
//  注释掉的那一行**只有写的人知道**,换个文件、过两周就没人说得清"这个组件
//  到底是没做完还是故意关掉的"。宏集中在这里,一眼能看全整机开了哪些功能,
//  而且 IDE 里 ctrl+点击能跳到定义。
//
//  ⚠ 裁掉的到底是什么 —— 这一点很容易搞错:
//    · **RAM 一定会省**。任务栈是 xTaskCreate 时从 12KB 堆里切走的,只要那个
//      xxx_tasks_init() 被调用就永久占着,跟任务干不干活无关。全开的话
//      wifi(512字)+ lock(320)+ finger(256)+ key_scan(256)+ key_action(128)
//      + cy9cm_scan(256) ≈ 1728 字 ≈ 6.9KB,占掉堆的一多半。
//    · **Flash 不用宏去省,链接器自己会省**。实测 map 文件里
//      "Removing Unused input sections" 一节已经删掉了 111 个没被引用的函数
//      (比如 NV400F_SendCmd、mg200_erase_all)。所以关掉组件省的是 RAM 和 CPU,
//      不是靠宏把代码从镜像里抠掉。
//
//  ⚠ 这些开关**不控制驱动文件是否参与编译**。i2c.c / MG200.c 这些源文件都还在
//    工程里(见 .eide/eide.yml),关掉开关只是不初始化、不建任务。
//    真要把整个文件从构建里去掉,得去 eide.yml 里摘。
// ══════════════════════════════════════════════════════════════════════════

// ── 互相独立的组件 ──

#define APP_USE_KEY     1   // PC13 物理按键 + 长按进 STANDBY(和密码无关)
#define APP_USE_WIFI    1   // WB2 联网:连 WiFi → MQTT → 收发云端下发
#define APP_USE_VOICE   1   // NV400F 语音播报(只做初始化;SendCmd 目前没人调)
#define APP_USE_LEARN   0   // FreeRTOS 学习演示(任务/队列/信号量/互斥量),业务不依赖它

// ── 有依赖关系的组件,关的顺序必须从后往前 ──
//
//      APP_USE_WIFI ┐
//      APP_USE_LOCK ┼→  APP_USE_LCD  →  (SPI + W25Q)
//      APP_USE_FINGER┘    显示屏          字库存在 W25Q 里
//        往屏幕上写字      显示组件
//
//      APP_USE_FINGER  →  APP_USE_LOCK  →  APP_USE_TOUCH
//       指纹比对通过      密码锁状态机      电容触摸板
//       走锁的开锁流程    驱动电机/LCD      密码从这儿来

#define APP_USE_TOUCH   1   // CY8CMBR3116 电容触摸板(密码的输入源)
#define APP_USE_LOCK    1   // 密码锁:触摸板输入 → 校验 → 驱动 L9110S(依赖 TOUCH)
#define APP_USE_FINGER  1   // MG200 指纹(依赖 LOCK:比对通过后走同一个开锁流程)
#define APP_USE_LCD     1   // 240x320 屏(依赖它的有 LOCK/FINGER/WIFI 的界面)

// ── 调试用的小开关 ──

#define APP_USE_MG200_DUMP  1   // 开机打印指纹模块里已录的指纹数量

// ══════════════════════════════════════════════════════════════════════════
//  编译期依赖检查
//
//  为什么要 #error 而不是让它自然编译失败:关错顺序的话,链接时报的是
//  "Undefined symbol lock_open",得顺藤摸瓜才能反应过来是自己关错了开关。
//  这里直接把人话摔在脸上。
// ══════════════════════════════════════════════════════════════════════════

#if APP_USE_LOCK && !APP_USE_TOUCH
#error "配置冲突:APP_USE_LOCK 需要 APP_USE_TOUCH —— 锁的密码是从触摸板来的"
#endif

#if APP_USE_FINGER && !APP_USE_LOCK
#error "配置冲突:APP_USE_FINGER 需要 APP_USE_LOCK —— 指纹比对通过后走的是锁的开锁流程"
#endif

// 下面三条是同一类:这三个组件都要往屏幕上写字,关了 LCD 它们就没有嘴了。
// 严格说这不是"不能编译"(画面调用会一起被 #if 掉),而是**功能残废**——
// 锁按了没反应、指纹认证看不到提示、配网不知道进行到哪一步。
// 与其让人上板debug半天,不如编译期直接拦下来。
#if APP_USE_LOCK && !APP_USE_LCD
#error "配置冲突:APP_USE_LOCK 需要 APP_USE_LCD —— 锁的输入界面是画在屏幕上的"
#endif

#if APP_USE_FINGER && !APP_USE_LCD
#error "配置冲突:APP_USE_FINGER 需要 APP_USE_LCD —— 指纹任务要往屏幕上写标题"
#endif

#if APP_USE_WIFI && !APP_USE_LCD
#error "配置冲突:APP_USE_WIFI 需要 APP_USE_LCD —— 配网过程要在屏幕上提示"
#endif

#endif // !__APP_CONFIG_
