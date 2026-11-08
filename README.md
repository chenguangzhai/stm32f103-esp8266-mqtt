# STM32F103 + ESP-01S MQTT 物联网节点

跑在 **STM32F103C8T6** 上的物联网终端：用 AT 指令驱动 **ESP-01S** 连 WiFi，
在 TCP 透传通道上自建 **MQTT 3.1.1** 客户端收发报文，周期上报传感器数据、订阅下行控制。

整条网络链路是**非阻塞**的——配网、建 TCP、进透传、掉线重连、MQTT 心跳全部由状态机推进，
主循环里没有一处 `HAL_Delay` 或忙等轮询在等网络。这是它和大多数教学 Demo 最大的区别：
`AT+CWJAP` 之后的每一个环节都会让出 CPU，OLED 刷新和按键响应不会被网络拖住。

## 功能

- **AT 表驱动状态机**：配网 → 建链 → 进透传，断线自动重连（不重新配 WiFi）
- **自建 MQTT 客户端**：CONNECT / SUBSCRIBE / PUBLISH / PINGREQ 收发全程非阻塞
- **周期上报 + 下行控制**：扁平 JSON，字段名与上位机网页端约定一致
- **DMA 环形接收**：USART2 收 ESP-01S 的数据走 DMA1_Channel6，主循环忙也不丢字节
- **OLED 状态页**：软件 I2C，2Hz 刷新不闪烁，脱机也能看出卡在哪一步

## 分层

分层的目的是**换一层不影响另一层**：

```
Application/
  BSP/
    board.c         板级服务：毫秒节拍、精确延时、非阻塞定时、指示灯
    UART/           串口通道封装（USART2 收 / USART1 日志）
    OLED/           SSD1306 驱动（软件 I2C）
  Middle/
    ESP8266/
      ESP8266.c     AT 指令表驱动状态机：只认 AT 指令，不知道 MQTT 存在
      mqtt_client.c 透传通道上的 MQTT 客户端：只认二进制，不知道底下是 WiFi 还是 4G
      mqtt/         Eclipse Paho MQTTPacket（只用序列化/反序列化）
      json/         cJSON
Core/               CubeMX 生成的 HAL 初始化（main / gpio / usart / dma）
Drivers/            STM32F1 HAL + CMSIS
Web/                浏览器端调试面板（mqtt.js over WebSocket）
Tools/              PC 端 AT 调试工具（串口助手，Python + tkinter）
```

两层之间唯一的接口是 `ESP8266_IsTransparent()`。想换成 4G 模组或换物联网云，
只需要替换 `ESP8266.c` 这一层，`mqtt_client.c` 一行都不用改。

## 硬件接线

| 功能 | 引脚 | 说明 |
| --- | --- | --- |
| 日志 USART1 | PA9 / PA10 | 115200 8N1，只发不收 |
| ESP-01S USART2 | PA2 / PA3 | 115200 8N1，RX 走 DMA1_Channel6 |
| OLED SCL / SDA | PB8 / PB9 | 软件 I2C，开漏 + 上拉 |
| 风扇指示灯 | 见 `board.c` | 由 `Led_Fan()` 封装，换脚只改一处 |

ESP-01S 供电必须 3.3V，且要能供出 300mA 以上的瞬时电流——供电不足的典型现象是
连上 WiFi 后几秒内反复重启。

## 通信协议

上行主题 `4G_<队名>_<赛场号>_<座位号>`，负载是扁平 JSON：

```json
{"temp": 25.2, "humi": 54, "light": 1762, "solitemp": "32",
 "knob1": 1.5, "knob2": 2.0, "mode": 0, "fan_status": 0, "rfid": ""}
```

下行主题 `Client_<队名>_<赛场号>_<座位号>`：

```json
{"fan_status": 1}
```

字段类型是网页端按名字取的，`light` 不能写成 `lux`，`solitemp` 是字符串不是数字。
端口 1883 只写在 `AT_CIPSTART` 宏里一处，避免两个地方各写一个数字改漏一个。

## 编译

用 [EIDE](https://em-ide.com/) 直接打开工程最省事（`.eide/` 已配好）。
也可以用 CMake + `arm-none-eabi-gcc`：

```bash
cmake --preset Debug
cmake --build build/Debug
```

烧录用 ST-Link 或 DAP-Link，SWD 接口。

编译前要改两处：

1. `Application/Middle/ESP8266/ESP8266.h` 顶部的 `WIFI_SSID` / `WIFI_PASS`
   填自己的 **2.4GHz** 热点（ESP8266 不支持 5G，连 5G 的现象是 `AT+CWJAP_DEF` 回 FAIL）
2. `Application/Middle/ESP8266/mqtt_client.h` 顶部的 `MQTT_CLIENT_ID` 和主题宏

## 配套工具

- `Web/index.html`：纯前端面板，mqtt.js over WebSocket（设备的 1883 是裸 TCP，
  浏览器连不了，面板走 8083 的 ws）。`cd Web && npm install && npx serve .`
- `Tools/at_assistant.py`：PC 端 AT 指令助手，实时回显 + 一键发指令 + HEX 收发。
  单文件 tkinter，读串口放在后台线程，界面不卡。
  Windows 现成的可执行文件在 [Releases](https://github.com/chenguangzhai/stm32f103-esp8266-mqtt/releases/latest)
  里，下载双击即用，不需要装 Python。

## 排查顺序

连不上时按这个顺序看，USART1 的日志能覆盖全部环节：

1. 模组有没有回 `ready`——没有就是接线/供电/波特率
2. `AT+CWJAP_DEF` 之后有没有 `WIFI GOT IP`
3. `AT+CIPSTATUS` 里是不是 `STATUS:5`（没连上）
4. MQTT CONNACK 的返回码——非 0 就是账号、ClientID 或 broker 的问题
