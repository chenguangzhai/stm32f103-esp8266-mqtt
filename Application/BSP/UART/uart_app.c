#include "uart_app.h"
#include "usart.h"					/* huart1 / huart2：HAL 只出现在这里，不进头文件 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>


/* 环形取模用 & (SIZE-1) 代替 %，所以长度必须是 2 的幂，这里编译期就拦住 */
#if ((UART_RX_BUF_SIZE & (UART_RX_BUF_SIZE - 1)) != 0)
#error "UART_RX_BUF_SIZE 必须是 2 的幂，否则环形下标会算错"
#endif

/* 一个串口对应一份，全部静态分配，不碰堆 */
typedef struct
{
	UART_HandleTypeDef *huart;					/* 对应的串口句柄 */
	uint8_t  buf[UART_RX_BUF_SIZE];				/* DMA 直接写入的环形缓冲 */
	volatile uint32_t wr_total;					/* 累计写入字节数（单调递增） */
	volatile uint32_t last_pos;					/* 上次回调时 DMA 的位置 */
	uint32_t rd_total;							/* 累计读取字节数（单调递增） */
	volatile uint8_t  idle_flag;				/* 收到 IDLE，说明一帧结束 */
	volatile uint32_t overflow;					/* 数据被覆盖的次数 */
	uint8_t  inited;							/* 这一格是否已被占用 */
} UartPort_t;

static UartPort_t s_port[UART_CH_NUM];

/* 通道号 → 串口句柄。加新串口只改这张表，调用点一行都不用动。 */
static UART_HandleTypeDef *const s_huart[UART_CH_NUM] =
{
	[UART_LOG] = &huart1,
	[UART_AT]  = &huart2,
};

/* 该通道要不要启动 DMA 接收。日志口只发不收，省 1KB RAM。 */
static const uint8_t s_rx_en[UART_CH_NUM] =
{
	[UART_LOG] = 0,
	[UART_AT]  = 1,
};

/**
  * 函    数：通道号转端口表项
  * 参    数：ch 逻辑通道
  * 返 回 值：端口指针，通道号越界返回 NULL
  */
static UartPort_t *Uart_At(uart_ch_t ch)
{
	if((ch < 0) || (ch >= UART_CH_NUM))
	{
		return NULL;
	}
	return &s_port[ch];
}

/**
  * 函    数：按句柄找到对应的端口
  * 参    数：huart 串口句柄
  * 返 回 值：端口指针，没注册过返回 NULL
  * 注    意：只给中断回调（拿到的就是 huart）用，对外接口一律走 Uart_At()。
  */
static UartPort_t *Uart_Find(UART_HandleTypeDef *huart)
{
	int i;

	if(huart == NULL)
	{
		return NULL;
	}
	for(i = 0; i < UART_CH_NUM; i++)
	{
		if(s_port[i].inited && (s_port[i].huart == huart))
		{
			return &s_port[i];
		}
	}
	return NULL;
}


#if UART_ECHO_DEBUG
/**
  * 函    数：把一段字节按可读形式打到指定串口，一条日志永远占一行
  * 参    数：port 输出到哪个串口
  * 参    数：tag  行首标记，如 "[TX" / "[RX"
  * 参    数：buf  要打印的数据
  * 参    数：len  数据长度
  * 返 回 值：无
  * 注    意：可打印字符原样输出；\r \n 打成两个字符，避免把一条日志拆成好几行；
  *           其余（0x00、中文、乱码）打成 \xNN。
  *           内部直接用 HAL_UART_Transmit，不走 Uart_Write，避免日志自己打自己。
  */
static void Uart_LogBytes(UART_HandleTypeDef *port, const char *tag,
                          const uint8_t *buf, uint16_t len)
{
	char     out[64];
	char     head[16];
	uint16_t k = 0;
	uint16_t i;
	uint8_t  c;

	if((port == NULL) || (tag == NULL) || (buf == NULL))
	{
		return;
	}

	/* 行首写成 [RX 42] 这样，长度直接摆在眼前，比原来那个假的 size 有用 */
	snprintf(head, sizeof(head), "%s %u] ", tag, (unsigned int)len);
	HAL_UART_Transmit(port, (uint8_t *)head, (uint16_t)strlen(head), UART_TX_TIMEOUT);

	for(i = 0; i < len; i++)
	{
		c = buf[i];

		if((c >= 0x20) && (c <= 0x7E))
		{
			out[k++] = (char)c;
		}
		else if(c == '\r')
		{
			out[k++] = '\\';
			out[k++] = 'r';
		}
		else if(c == '\n')
		{
			out[k++] = '\\';
			out[k++] = 'n';
		}
		else
		{
			out[k++] = '\\';
			out[k++] = 'x';
			out[k++] = "0123456789ABCDEF"[c >> 4];
			out[k++] = "0123456789ABCDEF"[c & 0x0F];
		}

		/* 最坏一个字节转义成 4 个字符，留 6 字节余量再刷出去 */
		if(k >= (sizeof(out) - 6))
		{
			HAL_UART_Transmit(port, (uint8_t *)out, k, UART_TX_TIMEOUT);
			k = 0;
		}
	}

	if(k > 0)
	{
		HAL_UART_Transmit(port, (uint8_t *)out, k, UART_TX_TIMEOUT);
	}

	HAL_UART_Transmit(port, (uint8_t *)"\r\n", 2, UART_TX_TIMEOUT);
}
#endif

/**
  * 函    数：启动（或重启）环形 DMA 接收
  * 参    数：p 端口指针
  * 返 回 值：UART_OK 成功，其它见头文件里的错误码
  * 注    意：只在初始化、出错恢复时调用。正常运行中 DMA 不会停，
  *           所以不需要「取完一帧再重启」这种动作。
  */
static int8_t Uart_RecvRestart(UartPort_t *p)
{
	if(p == NULL)
	{
		return UART_ERR_PARAM;
	}

	/* 上一次可能还没结束，先中止，否则 ReceiveToIdle 会返回 HAL_BUSY */
	if(p->huart->RxState != HAL_UART_STATE_READY)
	{
		HAL_UART_AbortReceive(p->huart);
	}

	/* 三个计数器必须一起归零。
	   DMA 重启后实际是从 p->buf[0] 开始写的，而 Uart_Read 用
	   wr_total / rd_total 的低位当下标。如果只清 last_pos 和 rd_total、
	   留着旧的 wr_total，读写下标就会永久错开一截：
	   读指针落在 DMA 还没写过的区域 → 读出一片 0；
	   绕回 0 附近 → 读到上一轮的旧数据。
	   这就是「buf = 空 size = 200」连刷几十行、以及回应串位的原因。 */
	p->wr_total  = 0;
	p->rd_total  = 0;
	p->last_pos  = 0;
	p->idle_flag = 0;

	if(HAL_UARTEx_ReceiveToIdle_DMA(p->huart, p->buf, UART_RX_BUF_SIZE) != HAL_OK)
	{
		/* 出错恢复时，HAL 那边可能还在异步中止 DMA 通道，第一次会返回 HAL_BUSY。
		   这里做一次阻塞式中止再重试，否则这个串口就再也收不到东西了。 */
		HAL_UART_AbortReceive(p->huart);

		p->wr_total  = 0;
		p->rd_total  = 0;
		p->last_pos  = 0;
		p->idle_flag = 0;

		if(HAL_UARTEx_ReceiveToIdle_DMA(p->huart, p->buf, UART_RX_BUF_SIZE) != HAL_OK)
		{
			return UART_ERR_STATE;
		}
	}
	return UART_OK;
}

/**
  * 函    数：绑定所有通道并启动接收
  * 参    数：无
  * 返 回 值：UART_OK 成功，其它见头文件里的错误码
  * 注    意：重复调用是安全的，已注册的通道直接跳过、不会清掉未读数据。
  *           哪些通道需要接收由 s_rx_en[] 决定，日志口只发不收。
  */
int8_t Uart_Init(void)
{
	int8_t  ret;
	int     i;

	for(i = 0; i < UART_CH_NUM; i++)
	{
		UartPort_t *p = &s_port[i];

		if(s_huart[i] == NULL)
		{
			continue;
		}
		if(p->inited)
		{
			continue;
		}

		memset(p, 0, sizeof(UartPort_t));
		p->huart  = s_huart[i];
		p->inited = 1;

		if(s_rx_en[i])
		{
			ret = Uart_RecvRestart(p);
			if(ret != UART_OK)
			{
				p->inited = 0;
				return ret;
			}
		}
	}
	return UART_OK;
}

/**
  * 函    数：注销一个通道，停止接收并释放表项
  * 参    数：ch 逻辑通道
  * 返 回 值：无
  */
void Uart_DeInit(uart_ch_t ch)
{
	UartPort_t *p = Uart_At(ch);

	if(p == NULL)
	{
		return;
	}

	if(s_rx_en[ch])
	{
		HAL_UART_AbortReceive(p->huart);
	}
	memset(p, 0, sizeof(UartPort_t));
}

/**
  * 函    数：查询缓冲里还有多少字节没读
  * 参    数：ch 逻辑通道
  * 返 回 值：可读字节数，0 表示没数据
  */
uint16_t Uart_Available(uart_ch_t ch)
{
	UartPort_t *p = Uart_At(ch);
	uint32_t avail;

	if(p == NULL)
	{
		return 0;
	}

	avail = p->wr_total - p->rd_total;
	if(avail > UART_RX_BUF_SIZE)
	{
		avail = UART_RX_BUF_SIZE;		/* 防御：理论上不会到这，除非溢出处理失效 */
	}
	return (uint16_t)avail;
}

/**
  * 函    数：从缓冲里取最多 len 个字节，取多少算多少
  * 参    数：ch   逻辑通道
  * 参    数：buf  取到的数据存放处
  * 参    数：len  最多取多少字节
  * 返 回 值：实际取到的字节数，0 表示没数据
  * 注    意：这是给「字节流」用的接口。MQTT 这类协议要几个字节就取几个，
  *           不用关心报文边界在哪。
  */
uint16_t Uart_Read(uart_ch_t ch, uint8_t *buf, uint16_t len)
{
	UartPort_t *p = Uart_At(ch);
	uint32_t avail;
	uint32_t rd;
	uint32_t i;

	if((p == NULL) || (buf == NULL) || (len == 0))
	{
		return 0;
	}

	avail = p->wr_total - p->rd_total;
	if(avail > UART_RX_BUF_SIZE)
	{
		avail = UART_RX_BUF_SIZE;
	}
	if(avail > len)
	{
		avail = len;
	}
	if(avail == 0)
	{
		return 0;
	}

	rd = p->rd_total;
	for(i = 0; i < avail; i++)
	{
		buf[i] = p->buf[(rd + i) & (UART_RX_BUF_SIZE - 1)];
	}
	p->rd_total = rd + avail;
	p->idle_flag = 0;			/* 这一帧已经交给应用层了，帧标志作废 */

#if UART_ECHO_DEBUG
	/* 打的是实际取到的 avail 个字节，不是调用方给的 len。
	   原来那句 printf("buf = %ssize = %u") 有两个毛病：
	   1) size 打的是 len，永远是调用方传的长度，跟真实收到多少无关；
	   2) %s 遇到 0x00 就截断，回应里的 \r\n 还会把一条日志拆成好几行。 */
	Uart_LogBytes(s_huart[UART_ECHO_CH], "[RX", buf, (uint16_t)avail);
#endif

	return (uint16_t)avail;
}

/**
  * 函    数：查询这一帧收完了没有，立即返回，不阻塞
  * 参    数：ch 逻辑通道
  * 返 回 值：1 有新的一帧可取，0 还没收完或串口没注册
  * 注    意：这个函数只读不清，真正的清除在 Uart_Read() 里做。
  *           所以「看到 1」和「取走」之间不会丢帧。
  *           主循环里放心反复调，开销只有一次函数调用。
  */
uint8_t Uart_FrameReady(uart_ch_t ch)
{
	UartPort_t *p = Uart_At(ch);

	return ((p != NULL) && (p->idle_flag != 0)) ? 1 : 0;
}

/**
  * 函    数：丢弃缓冲里还没读的数据
  * 参    数：ch 逻辑通道
  * 返 回 值：无
  * 注    意：读指针直接追平写指针，不清缓冲内容。DMA 一直在写，
  *           清内容没有意义，反而可能抹掉刚进来的新数据。
  */
void Uart_FlushRx(uart_ch_t ch)
{
	UartPort_t *p = Uart_At(ch);

	if(p == NULL)
	{
		return;
	}

	p->rd_total  = p->wr_total;
	p->idle_flag = 0;
}

/**
  * 函    数：查询数据被覆盖的次数
  * 参    数：ch 逻辑通道
  * 返 回 值：溢出次数，0 表示从没丢过数据
  * 注    意：这个值非 0 就说明主循环读得太慢，缓冲被写满绕圈了，
  *           要么调大 UART_RX_BUF_SIZE，要么提高读取频率。
  */
uint32_t Uart_GetOverflow(uart_ch_t ch)
{
	UartPort_t *p = Uart_At(ch);

	return (p == NULL) ? 0 : p->overflow;
}

/**
  * 函    数：发送一串数据
  * 参    数：ch  逻辑通道
  * 参    数：buf 要发的数据
  * 参    数：len 数据长度
  * 返 回 值：无
  */
void Uart_Write(uart_ch_t ch, const uint8_t *buf, uint16_t len)
{
	UART_HandleTypeDef *huart;

	if((ch < 0) || (ch >= UART_CH_NUM) || (s_huart[ch] == NULL))
	{
		return;
	}
	if((buf == NULL) || (len == 0))
	{
		return;
	}

	huart = s_huart[ch];

#if UART_ECHO_DEBUG
	/* 往「日志口以外」的串口发东西时，先在日志口复述一遍发出去的内容，
	   这样 [TX] 和紧随其后的 [RX] 就自然配成一对。
	   ch != UART_ECHO_CH 这个判断同时挡住了「日志自己打自己」的死递归。 */
	if(ch != UART_ECHO_CH)
	{
		Uart_LogBytes(s_huart[UART_ECHO_CH], "[TX", buf, len);
	}
#endif

	HAL_UART_Transmit(huart, (uint8_t *)buf, len, UART_TX_TIMEOUT);
}

/**
  * 函    数：自己封装的printf函数，可以指定发到哪个串口
  * 参    数：ch     逻辑通道
  * 参    数：format 格式化字符串
  * 参    数：...    可变的参数列表
  * 返 回 值：无
  */
void Uart_Printf(uart_ch_t ch, const char *format, ...)
{
	char String[100];				//定义字符数组
	va_list arg;					//定义可变参数列表数据类型的变量arg

	if((ch < 0) || (ch >= UART_CH_NUM) || (format == NULL))
	{
		return;
	}

	va_start(arg, format);			//从format开始，接收参数列表到arg变量
	/* 用vsnprintf不用vsprintf：结果超过100字节时会被截断，不会写穿栈 */
	vsnprintf(String, sizeof(String), format, arg);
	va_end(arg);					//结束变量arg

	/* 走 Uart_Write 而不是直接 HAL_UART_Transmit，
	   这样 AT 指令也能自动获得 [TX] 日志，业务层一行都不用改 */
	Uart_Write(ch, (uint8_t *)String, (uint16_t)strlen(String));
}

/**
  * 函    数：接收事件回调，IDLE / 半传输 / 传输完成都会进这里
  * 参    数：huart 触发的串口
  * 参    数：Size  当前 DMA 写指针的位置（不是本帧长度）
  * 返 回 值：无
  * 注    意：环形模式下 Size 会在 0..UART_RX_BUF_SIZE 之间循环，
  *           所以用「和上次位置的差值」来累加，而不是直接用 Size。
  *           这个函数由HAL在中断里调用，只做计数，别加耗时操作。
  */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
	UartPort_t *p = Uart_Find(huart);
	uint32_t delta;

	if(p == NULL)
	{
		return;
	}

	/* 收满整块时 HAL 给的是 BUF_SIZE，归到 0 处理，差值算法才正确 */
	if(Size >= UART_RX_BUF_SIZE)
	{
		Size = 0;
	}

	/* 环形增量：上次位置到当前位置走了多远，&(SIZE-1) 天然处理绕回 */
	delta = ((uint32_t)Size - p->last_pos) & (UART_RX_BUF_SIZE - 1);
	p->wr_total += delta;
	p->last_pos  = (uint32_t)Size;

	/* 只有 IDLE 才代表「这一帧发完了」，半传输和传输完成不算 */
	if(huart->RxEventType == HAL_UART_RXEVENT_IDLE)
	{
		p->idle_flag = 1;
	}

	/* 写超过读一整圈 = 读得太慢，旧数据已被覆盖，把被压掉的那段丢掉 */
	if((p->wr_total - p->rd_total) > UART_RX_BUF_SIZE)
	{
		p->overflow++;
		p->rd_total = p->wr_total - UART_RX_BUF_SIZE;
	}
}

/**
  * 函    数：串口错误回调，出错后重启接收，不然就再也收不到了
  * 参    数：huart 出错的串口
  * 返 回 值：无
  */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	UartPort_t *p = Uart_Find(huart);

	if(p == NULL)
	{
		return;
	}

	/* 溢出/帧错/噪声错都会停掉 DMA，不重启就彻底哑了 */
	Uart_RecvRestart(p);
}
