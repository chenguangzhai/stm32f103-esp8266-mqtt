# STM32F103 + ESP8266 MQTT 物联网节点

基于 **STM32F103C8T6 + ESP-01S(ESP8266)** 的 MQTT 物联网终端。网络层用 AT 指令表驱动状态机，
MQTT 协议层用 MQTTPacket 手工组包，**整条链路全程非阻塞** —— 主循环里没有任何一处
`HAL_Delay` 或忙等轮询在等网络。

## 功能

- ESP-01S 配网 → 建 TCP → 进透传，断线后自动重建连接（不重新配 WiFi）
- MQTT 3.1.1：CONNECT / SUBSCRIBE / PUBLISH / 心跳，收发都不阻塞主循环
- 周期性上报 JSON，订阅下行控制主题
- OLED 状态页 2Hz 刷新（软件 I2C），刷屏不闪烁
- USART2 走 DMA 环形缓冲接收，串口数据不会因为主循环忙而丢

## 目录结构

```
Application/
  BSP/
    board.c         板级基础服务：毫秒节拍、精确延时、非阻塞定时、指示灯
    UART/           串口通道封装（USART2 收 + USART1 日志）
    OLED/           OLED 驱动（软件 I2C，SSD1306）
  Middle/
    ESP8266/
      ESP8266.c     AT 指令表驱动状态机：配网、建链、进透传、重连
      mqtt_client.c 透传通道上的 MQTT 客户端（自建非阻塞收发）
      mqtt/          Eclipse Paho MQTTPacket（仅用其序列化/反序列化）
      json/          cJSON
Core/               CubeMX 生成的 HAL 初始化（main / gpio / usart / dma）
Drivers/            STM32F1 HAL + CMSIS
Web/                浏览器端调试面板（mqtt.js over WebSocket）
```

分层原则：`ESP8266.c` 只认 AT 指令、不知道 MQTT 存在；`mqtt_client.c` 只认二进制报文、
不知道底下是 WiFi 还是 4G。两者唯一的接口是 `ESP8266_IsTransparent()`，
换物联网云或换模组时只动其中一层。

## 快速开始

1. **填热点口令**（不进仓库）

   ```bash
   cd Application/Middle/ESP8266
   cp wifi_secret.example.h wifi_secret.h
   ```

   然后编辑 `wifi_secret.h` 填自己的 **2.4GHz** 热点（ESP8266 不支持 5G，
   连 5G 的现象是 `AT+CWJAP_DEF` 回 FAIL 或超时）。

2. **改 MQTT 参数**：`Application/Middle/ESP8266/mqtt_client.h` 顶部的
   `MQTT_CLIENT_ID`、`MQTT_TOPIC_POST`、`MQTT_TOPIC_SET`，
   命名规则是 `队名_赛场号_座位号`。

3. **编译**：用 [EIDE](https://em-ide.com/) 直接打开工程（推荐），
   或用 CMake + `arm-none-eabi-gcc`：

   ```bash
   cmake --preset Debug
   cmake --build build/Debug
   ```

4. **烧录**：ST-Link / DAP-Link 均可，SWD 接口。

## 硬件接线

| 功能 | 引脚 | 说明 |
| --- | --- | --- |
| 日志串口 USART1 | PA9 / PA10 | 115200 8N1，只发 |
| ESP-01S USART2 | PA2 / PA3 | 115200 8N1，RX 走 DMA1_Channel6 |
| OLED SCL / SDA | PB8 / PB9 | 软件 I2C，开漏输出 + 上拉 |
| 风扇指示灯 | 见 `board.c` | `Led_Fan()` 封装，换脚只改这一处 |

## 通信协议

上行主题 `4G_<队名>_<赛场号>_<座位号>`，负载为扁平 JSON：

```json
{"temp": 25.2, "humi": 54, "light": 1762, "solitemp": "32",
 "knob1": 1.5, "knob2": 2.0, "mode": 0, "fan_status": 0, "rfid": ""}
```

下行主题 `Client_<队名>_<赛场号>_<座位号>`，负载同样是扁平键值对：

```json
{"fan_status": 1}
{"mode": 0}
```

字段类型是网页端按名字取的，`light` 不能写成 `lux`，`solitemp` 是字符串而不是数字，
`fan_status` 不能简写成 `fan`（代码里保留了 `fan` 作为旧写法别名）。
端口 1883 写死在 `AT_CIPSTART` 宏里，只此一处，避免两个地方各写一个数字改漏。

## Web 调试面板

`Web/index.html` 是纯前端页面，用 mqtt.js 通过 WebSocket 连 broker
（设备的 1883 是裸 TCP，浏览器用不了，所以面板走 8083 的 ws）。

```bash
cd Web
npm install
npx serve .
```

打开后填自己的 broker 地址即可。`Web/verify.js` 是一个独立的命令行验证脚本，
可以直接在 Node 里跑收发热身，不用真的接板子。

## 调试建议

- 串口日志全部走 USART1，接 USB-TTL 就能看 AT 交互的每一步
- OLED 页显示当前网络状态 + MQTT 状态 + 最近一次上报，脱机也能判断卡在哪一环节
- 连不上的排查顺序：模组有没有回 `ready` → 有没有 `WIFI GOT IP` →
  `AT+CIPSTATUS` 的 `STATUS:5` 是不是没连上 → CONNACK 返回码
