#ifndef _LEARN_H_
#define _LEARN_H_

// ==================== 学习演示模块(与业务无关) ====================
// 本模块收纳所有 FreeRTOS 学习/演示代码:
//   串行打印互斥量 demo、vTaskDelay vs vTaskDelayUntil、计数信号量 demo、LCD 刷屏测试等
// 业务代码不要依赖本模块;每个 demo 的原理写在 learn.c 对应任务上方。
// 不想跑某个演示 → 在 learn_tasks_init() 里注释对应那行 xTaskCreate 即可。

void learn_tasks_init(void); // 创建打印互斥量/计数信号量 + 所有学习任务,须在 vTaskStartScheduler() 之前调用

#endif
