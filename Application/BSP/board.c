#include "board.h"
#include <stddef.h>         /* NULL */
#include "stm32f1xx_hal.h"
#include "main.h"           /* LED_Pin / LED_GPIO_Port，CubeMX 生成的 Label */

/* DWT 只初始化一次。不复位也不会错，只是第一次调用多跑几条指令 */
static uint8_t s_dwt_ready;

/**
  * 函    数：按 CPU 周期数空转等待
  * 参    数：cycles 要等的周期数
  * 返 回 值：无
  * 注    意：用无符号减法比较，CYCCNT 回绕（约 59 秒 @72MHz）也不会死循环。
  */
static void Delay_Cycles(uint32_t cycles)
{
	uint32_t start = DWT->CYCCNT;

	while((DWT->CYCCNT - start) < cycles)
	{
	}
}

/**
  * 函    数：使能 DWT 周期计数器
  * 参    数：无
  * 返 回 值：无
  * 注    意：DWT 属于内核调试组件，不需要 CubeMX 配外设、不占引脚。
  *           必须先置 DEMCR.TRCENA 才能给 CYCCNT 计数。
  */
static void DWT_EnsureReady(void)
{
	if(s_dwt_ready != 0U)
	{
		return;
	}
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	s_dwt_ready = 1U;
}

/**
  * 函    数：读取开机以来的毫秒数
  * 参    数：无
  * 返 回 值：毫秒计数
  * 注    意：现在直接取 HAL 的 SysTick 计数（HAL_Init 里配成 1ms）。
  *           约 49.7 天回绕一次，所以调用方一律用「当前 - 上次 >= 间隔」的差值法，
  *           不能用「计数等于某值」来判断。
  *           以后要把节拍源换掉（比如自己开定时器），只改这一个函数。
  */
uint32_t Board_Millis(void)
{
	return HAL_GetTick();
}

/**
  * 函    数：微秒级延时
  * 参    数：us 要延时的微秒数
  * 返 回 值：无
  * 注    意：按 1000us 分段，避免 cycles 在 32 位里溢出。
  *           这是忙等，CPU 全程占着；只适合几十微秒到几毫秒的短延时
  *           （单总线器件时序、复位脉冲这类）。
  */
void Delay_Us(uint32_t us)
{
	const uint32_t cycles_per_us = SystemCoreClock / 1000000U;

	DWT_EnsureReady();

	while(us > 0U)
	{
		uint32_t chunk = (us > 1000U) ? 1000U : us;

		Delay_Cycles(chunk * cycles_per_us);
		us -= chunk;
	}
}

/**
  * 函    数：毫秒级延时
  * 参    数：ms 要延时的毫秒数
  * 返 回 值：无
  * 注    意：没有直接调 HAL_Delay，而是走 Delay_Us，两个原因：
  *           1. HAL_Delay 靠 SysTick 中断推进，中断被关了或进了中断里就永远等不到；
  *              DWT 版本不依赖中断，任何上下文都能用。
  *           2. 精度统一，不会出现「Ms 准、Us 也准，但两者的基准不同」。
  */
void Delay_Ms(uint32_t ms)
{
	while(ms > 0U)
	{
		uint32_t chunk = (ms > 10U) ? 10U : ms;

		Delay_Us(chunk * 1000U);
		ms -= chunk;
	}
}

/**
  * 函    数：点亮/熄灭风扇状态指示灯
  * 参    数：on 1 = 亮，0 = 灭
  * 返 回 值：无
  * 注    意：本板 LED 接在 VCC 与 GPIO 之间，低电平点亮，所以要取反。
  *           极性只在这一个函数里出现 —— 换底板接法反了，改这里一行，
  *           上面所有调用点（mqtt_client.c 里的风扇状态、心跳闪烁）一个字都不用动。
  */
void Led_Fan(uint8_t on)
{
	HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, (on != 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

/**
  * 函    数：翻转风扇状态指示灯
  * 参    数：无
  * 返 回 值：无
  * 注    意：翻转不用管极性，状态取反就对了。
  */
void Led_Fan_Toggle(void)
{
	HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
}

/**
  * 函    数：把定时基准重置到当前时刻
  * 参    数：last 时间基准变量（必须是 static 或结构体成员，函数内部不保存状态）
  * 返 回 值：无
  * 注    意：启动瞬间、状态切换时调一次，让下一个周期从此刻重新算。
  */
void Timer_Reset(uint32_t *last)
{
	if(last == NULL)
	{
		return;
	}
	*last = Board_Millis();
}

/**
  * 函    数：周期定时是否到点，到点后自动推进基准
  * 参    数：last   时间基准变量
  * 参    数：period 周期，毫秒
  * 返 回 值：1 到点（该执行了），0 还没到
  * 注    意：基准取「当前时刻」而不是「上次 + period」，所以主循环卡一下也不会
  *           补触发好几次（对上报来说，一次发好几条比晚一点更糟）。
  *           代价是长期有几十毫秒级漂移，本项目不需要精确定时。
  *           回绕安全：用的是无符号减法，Board_Millis 约 49.7 天绕一次也不受影响。
  */
uint8_t Timer_Expired(uint32_t *last, uint32_t period)
{
	uint32_t now;

	if(last == NULL)
	{
		return 0U;
	}

	now = Board_Millis();
	if((now - *last) < period)
	{
		return 0U;
	}

	*last = now;
	return 1U;
}

/**
  * 函    数：判断距基准是否已超过 timeout，不改动基准
  * 参    数：last    时间基准变量
  * 参    数：timeout 超时时间，毫秒
  * 返 回 值：1 已超时，0 还在等
  * 注    意：给「等某个回应」用（CONNACK、心跳、AT 指令返回）。
  *           基准什么时候重置由调用方决定 —— 收到响应后要重新计时，
  *           不能由这个函数代劳，否则「等到了」和「等超时了」会混在一起。
  */
uint8_t Timer_Timeout(uint32_t *last, uint32_t timeout)
{
	if(last == NULL)
	{
		return 0U;
	}
	return ((Board_Millis() - *last) >= timeout) ? 1U : 0U;
}
