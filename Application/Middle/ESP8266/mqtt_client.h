/**
  * MQTT 客户端（跑在 ESP8266 透传通道之上）
  *
  * 分工：
  *   ESP8266.c     负责「让模块连上网并进入透传」——它只会说 AT 指令
  *   mqtt_client.c 负责「在透传通道上收发 MQTT 报文」——它只会说二进制
  *   两者唯一的接口是 ESP8266_IsTransparent()，方向是 mqtt_client → ESP8266，
  *   也就是说 ESP8266.c 完全不知道 MQTT 的存在，以后换平台/换物联网云只改本文件。
  *
  * 为什么不用 Paho 的 MQTTPacket_read()：
  *   它要求调用方提供一个「给够 N 字节才返回」的 getfn，本质是阻塞读取。
  *   在透传通道上，报文的字节是陆续到的，阻塞等待会把整个主循环卡死；
  *   更糟的是「迟迟不到」时没有退出条件，程序就吊在那里。
  *   本文件把读取拆成「能取多少取多少 + 攒够一个完整报文再解析」，全程非阻塞。
  *
  * 配置：换产品/换设备/换平台，只改下面这一段宏。
  */
#ifndef __MQTT_CLIENT_H
#define __MQTT_CLIENT_H

#include <stdint.h>

/* ==================== 平台参数（比赛服务器）====================
   2024 年第六届任务书第 9 页给出的参数，原样搬来：
       IP 111.230.92.227:1883，用户名 admin，密码 admin123。

   和 OneNET 最大的区别：这里没有 token，就是普通用户名口令；
   连接成功与否只看 CONNACK 的返回码，没有产品/设备资源校验。
   端口写在 ESP8266.h 的 AT_CIPSTART 里，一处定义。 */
#define MQTT_CLIENT_ID      "team_1_1"      /* 队名_赛场号_座位号，改成自己的队名 */
#define MQTT_USERNAME       "admin"
#define MQTT_PASSWORD       "admin123"

/* 主题：任务书第 9 页第 4 条要求「与赛场号+座位号关联」，
   赛场 1 座位 1 就是 4G_team_1_1 / Client_team_1_1。

   ⚠ 拼写陷阱：任务书原文把 Client 写成了 "Clinet"（第 378/382/385 行），
   而 MQTTX 里用的是 Client_team_1_1。谁对不知道，所以两个都订。
   理由：主题写错的症状是「发送正常、永远收不到控制指令」，
   查起来会先怀疑网络、怀疑报文、怀疑 broker 权限，唯独很难想到是拼写。
   多订一个主题只多占十几字节报文，换掉一整类排查时间，很划算。

   上行只发一个：服务器端订阅哪个由 web 端决定，设备多发一份只会重复。 */
#define MQTT_TOPIC_POST      "4G_team_1_1"      /* 设备 → 服务器（数据上行） */
#define MQTT_TOPIC_SET       "Client_team_1_1"  /* 服务器 → 设备（控制下行） */
#define MQTT_TOPIC_SET_ALT   "Clinet_team_1_1"  /* 同上，任务书原文的拼法，一并订阅 */

/* 心跳周期（秒）。broker 在 1.5 倍周期内收不到任何报文就断开，
   代码在 1/2 周期发 PINGREQ，留一次丢包重试的余量。 */
#define MQTT_KEEPALIVE_S    60U

/* ==================== 是否自动周期上报 ====================
   1 = 上线后立刻报一次，之后每 MQ_PUB_MS（5 秒）报一次。
   0 = 不自动上报，只保持在线、只处理下行控制。
   手工上报随时可用：MQTT_Publish_Text() 不受这个开关影响。 */
#define MQ_AUTO_REPORT      1

/* ==================== 是否用模拟数据 ====================
   1 = 还没接传感器，每轮上报前按开机时长算出一组缓慢变化的数，
       让网页端能看到「数据在动」。范围都是合理值，不是随机跳变。
   0 = 用真实采集值。接上传感器后：先把采集结果写进 MQTT_Status()
       返回的结构体，再把这个开关改成 0。

   注意这个开关不改变上报格式，只改变数值来源 —— 所以网页端
   在 1 和 0 两种状态下都不用改。 */
#define MQ_SIM_DATA         1

/* ==================== 上下行数据格式 ====================
   上行（发到 4G_team_1_1），字段名与网页端严格一致：

     {"temp":25.5,"humi":60,"light":1000,"solitemp":"32",
      "knob1":1.5,"knob2":3.2,"mode":1,"fan_status":1,"rfid":"A1B2C3"}

     temp        温度，浮点，1 位小数
     humi        湿度，整数
     light       光照度，整数          ← 注意不是 lux
     solitemp    土壤温度，**字符串**  ← 注意是 "32" 不是 32
     knob1/knob2 两路旋钮电压（V），1 位小数
     mode        1=自动 0=手动
     fan_status  1=风扇运转 0=停止     ← 注意不是 fan
     rfid        刷卡 ID，字符串

   下行（从 Client_team_1_1 / Clinet_team_1_1 收），只认数值型字段：

     {"fan_status":1}    启停风扇（也接受 "fan"，老写法）
     {"mode":0}          切手动 / 自动

   可以只带其中一个字段，未出现的字段保持原值。 */

void    MQTT_Client_Loop(void);     /* 非阻塞，主循环里反复调用 */
uint8_t MQTT_IsOnline(void);        /* 1 = 已连上且已订阅 */
void    MQTT_Client_Reset(void);    /* 丢弃当前会话状态，下次 Loop 重新握手 */

/**
  * 函    数：MQTT_Publish_Text —— 把一段现成的字符串发到上行主题
  * 参    数：payload / len —— 负载内容与长度（不必以 '\0' 结尾）
  * 返 回 值：1 = 已发出，0 = 没发（不在线 / 装不下）
  * 注    意：内部已做在线判断。给以后接传感器时手工上报用，
  *           不必再关心 MQTT 报文怎么拼。
  */
uint8_t MQTT_Publish_Text(const char *payload, uint16_t len);

/* ---------- 设备侧状态，供业务层读写 ----------
   这个结构体就是「要上报的那一包」的镜像，字段与 JSON 一一对应。
   接传感器以后：采集结果写进这里（MQTT_Status()->temp = ...），
   再把 MQ_SIM_DATA 改成 0，上报自然带上真值，上报代码一行不用动。 */
typedef struct {
    double  temp;       /* 温度 ℃，1 位小数 */
    double  humi;       /* 湿度 %，整数 */
    double  light;      /* 光照度 lx，整数 */
    int32_t solitemp;   /* 土壤温度 ℃（上报时转成字符串） */
    double  knob1;      /* 旋钮 1 电压 V，1 位小数 */
    double  knob2;      /* 旋钮 2 电压 V，1 位小数 */
    uint8_t mode;       /* 1=自动 0=手动 */
    uint8_t fan_status; /* 1=风扇运转 0=停止 */
    char    rfid[16];   /* 刷卡 ID 字符串 */
} mqtt_status_t;

mqtt_status_t *MQTT_Status(void);

#endif
