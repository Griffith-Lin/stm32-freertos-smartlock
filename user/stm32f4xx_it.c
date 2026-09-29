/**
  ******************************************************************************
  * @file    Project/STM32F4xx_StdPeriph_Templates/stm32f4xx_it.c 
  * @author  MCD Application Team
  * @version V1.8.1
  * @date    27-January-2022
  * @brief   Main Interrupt Service Routines.
  *          This file provides template for all exceptions handler and 
  *          peripherals interrupt service routine.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2016 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
// 这里原来还有一行 #include "main.h" —— 是白加的。
// stm32f4xx_it.c 里的 6 个异常处理(NMI/HardFault/MemManage/BusFault/
// UsageFault/SVC)函数体全是 while(1),一个外部符号都不引用。
// 而 main.h 当时是个 include 了 20 个外设头文件的"万能头",等于让整个
// 中断向量表那份翻译单元白白依赖了 WiFi / 指纹 / 密码锁的全部声明。
// stm32f4xx_it.h 里已经有 stm32f4xx.h,够用了。
#include "stm32f4xx_it.h"

/** @addtogroup Template_Project
  * @{
  */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/

/******************************************************************************/
/*            Cortex-M4 Processor Exceptions Handlers                         */
/******************************************************************************/

/**
  * 🔴 L0 生死线代码 —— 中断向量表入口:能进 NMI 说明时钟失效/硬件告警级异常;现在是空函数,但往里加任何非中断安全代码都会把最后的现场搞丢
  * @brief  This function handles NMI exception.
  * @param  None
  * @retval None
  */
void NMI_Handler(void)
{
}

/**
  * 🔴 L0 生死线代码 —— 硬故障的"停尸房":while(1) 是刻意留的,好让 SWD 连上去对着 CFSR/HFSR 倒查崩溃点;改成"打印后返回"就会带着坏现场继续跑
  * @brief  This function handles Hard Fault exception.
  * @param  None
  * @retval None
  */
void HardFault_Handler(void)
{
  /* Go to infinite loop when Hard Fault exception occurs */
  while (1)
  {
  }
}

/**
  * 🔴 L0 生死线代码 —— MPU 访存违例的落点,与 HardFault 共用现场寄存器;这里的死循环是保护失效后的兜底,改动前要先吃透 Cortex-M4 的异常返回行为
  * @brief  This function handles Memory Manage exception.
  * @param  None
  * @retval None
  */
void MemManage_Handler(void)
{
  /* Go to infinite loop when Memory Manage exception occurs */
  while (1)
  {
  }
}

/**
  * 🔴 L0 生死线代码 —— 总线错误落点(访问没开时钟的外设、野指针等),本工程外设初始化顺序错时最容易撞进来;停住是为了保住现场而不是偷懒
  * @brief  This function handles Bus Fault exception.
  * @param  None
  * @retval None
  */
void BusFault_Handler(void)
{
  /* Go to infinite loop when Bus Fault exception occurs */
  while (1)
  {
  }
}

/**
  * 🔴 L0 生死线代码 —— 非法指令/非对齐访问等用法错误的落点,属于异常处理类;改它等于改系统崩溃时唯一可观测的那一环
  * @brief  This function handles Usage Fault exception.
  * @param  None
  * @retval None
  */
void UsageFault_Handler(void)
{
  /* Go to infinite loop when Usage Fault exception occurs */
  while (1)
  {
  }
}

/**
  * @brief  This function handles SVCall exception.
  * @param  None
  * @retval None
  */
// void SVC_Handler(void)
// {
// }

/**
  * 🔴 L0 生死线代码 —— 调试监控异常入口,正常不触发;在它里面做有副作用的动作会让调试器会话本身的行为不可预期
  * @brief  This function handles Debug Monitor exception.
  * @param  None
  * @retval None
  */
void DebugMon_Handler(void)
{
}

/**
  * @brief  This function handles PendSVC exception.
  * @param  None
  * @retval None
  */
// void PendSV_Handler(void)
// {
// }

/**
  * @brief  This function handles SysTick Handler.
  * @param  None
  * @retval None
  */
// void SysTick_Handler(void)
// {
  
// }

/******************************************************************************/
/*                 STM32F4xx Peripherals Interrupt Handlers                   */
/*  Add here the Interrupt Handler for the used peripheral(s) (PPP), for the  */
/*  available peripheral interrupt handler's name please refer to the startup */
/*  file (startup_stm32f4xx.s).                                               */
/******************************************************************************/

/**
  * @brief  This function handles PPP interrupt request.
  * @param  None
  * @retval None
  */
/*void PPP_IRQHandler(void)
{
}*/

/**
  * @}
  */ 


