/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "ESP8266.h"
#include "mqtt_client.h"
#include "OLED.h"
#include "board.h"          /* Timer_Expired：刷屏节拍，不直接用 HAL_GetTick */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void oled_show_status(void);   /* 定义在 USER CODE 4，主循环里要先用 */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  Uart_Init();			/* 绑定各通道并启动 DMA 环形接收（USART2 收，USART1 只发） */

  /* OLED 必须在 MX_GPIO_Init() 之后初始化：PB8/PB9 是 gpio.c 里配好的
     开漏输出 + 上拉，软件 I2C 就靠这两根线。 */
  OLED_Init();
  OLED_Clear();
  OLED_ShowString(16, 16, (char *)"IoT Panel", OLED_8X16);
  OLED_ShowString(10, 42, (char *)"booting...", OLED_6X8);
  OLED_Update();

  printf("USART2 DMA 环形接收已启动\r\n");
  HAL_Delay(1000);				/* 等待 ESP01S 上电自检完毕，避免丢掉开机的 AT+RST 响应 */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    ESP8266_Loop();       /* 网络层：配网 + 进透传，没进透传时内部只做这一步 */
    MQTT_Client_Loop();   /* 应用层：通道就绪后才真正干活，否则立刻返回 */
    oled_show_status();   /* 显示层：2Hz 把当前状态刷到屏上，内部自己控节拍 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/**
  * 函    数：fmt_1dp —— 把浮点数写成一位小数的字符串
  * 参    数：v —— 数值；out/n —— 输出缓冲
  * 说    明：不用 OLED_ShowFloatNum，因为它会先打一个 +/- 号占掉一格宽度，
  *           温度、电压都是正数，那一格纯属浪费（屏幕一行只有 21 格）。
  *           也不用 "%.1f"：虽然本工程链了 _printf_float 能打，但这里没必要
  *           为两个字段把浮点格式化整个拖进来。
  */
static void fmt_1dp(char *out, uint32_t n, double v)
{
    int32_t s = (int32_t)((v < 0.0) ? (v * 10.0 - 0.5) : (v * 10.0 + 0.5));

    if (s < 0)
    {
        snprintf(out, n, "-%ld.%01ld", (long)((-s) / 10), (long)((-s) % 10));
    }
    else
    {
        snprintf(out, n, "%ld.%01ld", (long)(s / 10), (long)(s % 10));
    }
}

/**
  * 函    数：oled_show_status
  * 说    明：把「当前这一份要上报的状态」画到 OLED 上。
  *
  *           数据源就是 MQTT_Status() —— 和上报 JSON 用的同一个结构体，
  *           所以屏幕上看到什么、线上发出去的就是什么，不会出现两边对不上
  *           还要猜是哪边错的情况。
  *
  *           刷新率定 2Hz：屏幕看着是实时的，又不至于每圈主循环都去推
  *           1KB 显存（软件 I2C 刷满一屏约 20~30ms，刷太勤会把 MQTT 的
  *           收发节奏挤歪）。
  *
  *           每行都用固定宽度（%6s / %5d / %-6s）：
  *           OLED 显存不清屏直接覆盖，宽度一变上次的字尾就会留在屏上，
  *           出现 "26.5 C0" 这种残留。
  */
static void oled_show_status(void)
{
    static uint32_t s_last;
    char  line[48];
    char  t[16], k1[16], k2[16];
    mqtt_status_t *st = MQTT_Status();

    if (!Timer_Expired(&s_last, 500U))
    {
        return;
    }

    fmt_1dp(t,  sizeof(t),  st->temp);
    fmt_1dp(k1, sizeof(k1), st->knob1);
    fmt_1dp(k2, sizeof(k2), st->knob2);

    snprintf(line, sizeof(line), "TEMP :%6s C", t);
    OLED_ShowString(0, 0, line, OLED_6X8);

    snprintf(line, sizeof(line), "HUMI :%5d %%", (int)st->humi);
    OLED_ShowString(0, 8, line, OLED_6X8);

    snprintf(line, sizeof(line), "LIGHT:%5d lx", (int)st->light);
    OLED_ShowString(0, 16, line, OLED_6X8);

    snprintf(line, sizeof(line), "SOIL :%5d C", (int)st->solitemp);
    OLED_ShowString(0, 24, line, OLED_6X8);

    snprintf(line, sizeof(line), "K1:%sV  K2:%sV", k1, k2);
    OLED_ShowString(0, 32, line, OLED_6X8);

    snprintf(line, sizeof(line), "MODE:%-6s FAN:%-3s",
             (st->mode != 0) ? "AUTO" : "MANUAL",
             (st->fan_status != 0) ? "ON" : "OFF");
    OLED_ShowString(0, 40, line, OLED_6X8);

    snprintf(line, sizeof(line), "RFID: %s", st->rfid);
    OLED_ShowString(0, 48, line, OLED_6X8);

    /* 最后一行专门用来看网络：现场排查时「屏幕显示 LINKING」和
       「屏幕正常但网页没数据」是两类完全不同的故障，一眼就能分开 */
    OLED_ShowString(0, 56,
                    MQTT_IsOnline() ? (char *)"MQTT: ONLINE"
                                    : (char *)"MQTT: LINKING...",
                    OLED_6X8);

    OLED_Update();
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
