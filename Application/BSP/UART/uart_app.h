/**
  * 串口收发封装：环形缓冲 + DMA 循环接收，DMA 永不停止
  *
  * 设计目标：一套代码管所有串口。将来接 WiFi / LoRa / NB-IoT / 4G 都直接调这里，
  *           换模块只改协议层，收发层一个字不用动。
  *
  * 和「收一帧停一次」的区别：
  *   旧方案一帧收完 DMA 就停，主循环来不及取，这段时间的字节全丢；
  *   本方案 DMA 一直在跑，收和取彻底解耦，主循环慢一点只是数据在缓冲里多待一会儿。
  *
  * 用法：
  *   Uart_Init();                              // 一次性绑定所有通道并启动接收
  *   uint8_t buf[64];
  *   uint16_t n = Uart_Read(UART_AT, buf, sizeof(buf));   // 按需取 N 字节
  *
  * 为什么是「通道号」而不是「传 huart」：
  *   传 &huart2 的话，调用方必须看见 UART_HandleTypeDef，就得 include usart.h，
  *   usart.h 又拖进 stm32f1xx_hal.h —— 一个协议层的 .c 就这样拿到了整个 HAL。
  *   改成通道号后，本头文件只依赖 stdint.h，谁 include 它都不会被迫认识 HAL。
  *   具体哪个通道对应哪个 huart，只写在 uart_app.c 的 s_huart[] 里。
  */
#ifndef __UART_APP_H
#define __UART_APP_H

#include <stdint.h>

/* 逻辑通道。加新串口（LoRa / 4G）就在末尾插一个，然后去 uart_app.c 的 s_huart[] 补一行 */
typedef enum
{
	UART_LOG = 0,		/* USART1：日志口，只发不收 */
	UART_AT  = 1,		/* USART2：ESP-8266 业务口 */
	UART_CH_NUM			/* 通道总数，必须放最后 */
} uart_ch_t;

/* 每个串口接收环形缓冲的字节数。
   必须是 2 的幂（内部用 & 取模，比 % 快）。
   1024 足够放一条几百字节的 MQTT 报文，RAM 紧张时可以降到 512。 */
#define UART_RX_BUF_SIZE    1024

/* 发送超时（毫秒） */
#define UART_TX_TIMEOUT     1000

/* ==================== 调试回显 ====================
   1 = 把 Uart_Read() 取到的数据原样转发到 UART_ECHO_CH 打印
   0 = 关闭，这段代码在编译期整段消失，零运行时开销

   调试时开着，串口助手里能直接看到从业务口收到的每一个字节；
   正式比赛/发布前改成 0，不占带宽也不拖慢主循环。 */
#define UART_ECHO_DEBUG     1

/* 回显输出到哪个通道，默认日志口 */
#define UART_ECHO_CH        UART_LOG

/* 函数返回值 */
#define UART_OK             0
#define UART_ERR_PARAM      (-1)    /* 参数为空 */
#define UART_ERR_STATE      (-3)    /* HAL 状态异常，启动接收失败 */

int8_t   Uart_Init(void);
void     Uart_DeInit(uart_ch_t ch);

uint16_t Uart_Available(uart_ch_t ch);
uint8_t  Uart_FrameReady(uart_ch_t ch);
uint16_t Uart_Read(uart_ch_t ch, uint8_t *buf, uint16_t len);
void     Uart_FlushRx(uart_ch_t ch);
uint32_t Uart_GetOverflow(uart_ch_t ch);

void     Uart_Write(uart_ch_t ch, const uint8_t *buf, uint16_t len);
void     Uart_Printf(uart_ch_t ch, const char *format, ...);

#endif
