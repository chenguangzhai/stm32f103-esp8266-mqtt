/**
  * 板级基础服务：节拍、延时、非阻塞定时、指示灯
  *
  * 为什么要有这一层：
  *   上层（Middle / App）只要一碰 HAL_GetTick、HAL_Delay、HAL_GPIO_WritePin，
  *   就得 include 整个 HAL，分层立刻被打穿，换底板时还要跟着改。
  *   这里把「毫秒从哪来」「延时多久」「哪颗灯代表什么」收成几个函数，
  *   上层只认函数名，不认引脚号和寄存器。
  *
  * 本文件只依赖 <stdint.h>，引它不会带进任何 HAL 头。
  */
#ifndef __BOARD_H
#define __BOARD_H

#include <stdint.h>

/* 一、节拍：判超时、算间隔统一用这个，差值法，不怕回绕 */
uint32_t Board_Millis(void);

/* 二、延时：基于 DWT 周期计数器，中断里也能用（但别在中断里长延时） */
void Delay_Ms(uint32_t ms);
void Delay_Us(uint32_t us);

/* 三、指示灯：on=1 表示「亮」，高低电平由 board.c 内部换算 */
void Led_Fan(uint8_t on);
void Led_Fan_Toggle(void);

/* 四、非阻塞定时：主循环每圈调一次，到点才返回 1，不睡、不占 CPU
 *
 *      static uint32_t t;                            // 状态必须由调用方保存
 *      if (Timer_Expired(&t, 500U)) {                // 每 500ms 执行一次
 *          Led_Fan_Toggle();
 *      }
 *
 *   注意：Delay_* 是硬件时序用的忙等，业务上的「等一会儿」一律用这两个函数。
 */
void    Timer_Reset(uint32_t *last);
uint8_t Timer_Expired(uint32_t *last, uint32_t period);
uint8_t Timer_Timeout(uint32_t *last, uint32_t timeout);

#endif
