/* ============================================================================
 * 文件：mqtt_client.c
 * 作用：在 ESP8266 透传通道上跑 MQTT 3.1.1（连服务器 → 订阅 → 保活 → 收下行）
 *
 * 前置条件：ESP8266.c 已经把模块带进透传模式（ESP8266_IsTransparent() == 1），
 *           此时 USART2 上跑的是裸 TCP 字节流，没有任何 AT 前缀、也没有 +IPD 头。
 *
 * 流程（每一步都不阻塞，靠 Board_Millis 差值判超时）：
 *
 *   MQS_IDLE ──通道就绪──> MQS_WAIT_CONNACK ──CONNACK rc=0──> MQS_WAIT_SUBACK
 *                              │                                   │
 *                              └── 超时/rc≠0 ──┐        SUBACK ────┴──> MQS_ONLINE
 *                                              │                          │
 *                                         MQS_BACKOFF <── 超时 ───────────┘
 *
 * 为什么不像参考工程那样用 MQTTPacket_read()：
 *   它的长度参数是个「必须给够 N 字节」的阻塞回调，透传通道上字节陆续到达，
 *   阻塞会让主循环停摆；而且字节迟迟不来时它没有退出条件。
 *   这里改成「能取多少取多少，攒够一个完整报文再解析」，规则只有两条：
 *     1) 首字节的高 4 位是报文类型，本状态不认识就丢掉 1 字节再试（顺便吃掉横幅残留）
 *     2) 长度字段收全后才算得出整包长度，收满了才交给解析函数
 * ========================================================================== */

#include "mqtt_client.h"
#include "ESP8266.h"
#include "MQTTPacket.h"
#include "cJSON.h"
#include "board.h"          /* 节拍/定时/灯：都不碰 HAL */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>         /* free()，cJSON_PrintUnformatted 返回的串要还回去 */

/* ---------- 缓冲 ----------
   RX 512：够放一条带几十字节 JSON 的下行报文；透传通道上单包超过这个尺寸
           只可能是我们自己的解析已经错位，见 mq_rx_pump() 里的兜底。
   TX 320：CONNECT 约 60 字节（用户名口令很短，没有 OneNET 那种长 token），
           SUBSCRIBE 订两个主题约 60 字节，上报约 90 字节。余量很足。 */
#define MQ_RX_BUF           512
#define MQ_TX_BUF           320

/* ---------- 超时（毫秒）---------- */
#define MQ_CONNACK_MS       5000    /* 发完 CONNECT 等 CONNACK */
#define MQ_SUBACK_MS        5000    /* 发完 SUBSCRIBE 等 SUBACK */
#define MQ_BACKOFF_MS       5000    /* 失败后歇一会儿再重来，别把 broker 当靶子 */
#define MQ_PING_MS          ((MQTT_KEEPALIVE_S / 2U) * 1000U)
#define MQ_PUB_MS           5000    /* 上报周期。传感器读数秒级变化，5 秒足够；
                                       赛题要求 web 端「实时显示」，再快也没意义 */

/* 多久没收到任何一个合法报文，就认定链路已死。
   取 2 个心跳周期 + 5 秒余量：正常链路每 MQ_PING_MS 必有一个 PINGRESP，
   连续两个没来基本可以排除「刚好这一包丢了」这种巧合。
   透传通道没有断线中断 —— 网线拔了、AP 关了、模块自己掉线，
   本地全都感知不到，只能靠这个静默时长判死。 */
#define MQ_DEAD_MS          (MQ_PING_MS * 2U + 5000U)

typedef enum {
    MQS_IDLE = 0,       /* 等通道就绪 / 等退避结束 */
    MQS_WAIT_CONNACK,   /* CONNECT 已发，等服务器认账 */
    MQS_WAIT_SUBACK,    /* SUBSCRIBE 已发，等订阅确认 */
    MQS_ONLINE,         /* 在线：可发上报、可收下行、定时心跳 */
    MQS_BACKOFF,        /* 失败了，歇一会儿再从头来 */
} mq_state_t;

/* 收到一包但还没解析的原始字节。
   +8 是留给尾部补零的：长度字段是变长的，解码函数会连读最多 4 个字节，
   在缓冲末尾补零能让它「读到 0 就停」，不会越界也不会读到上一包的残渣。 */
static uint8_t  s_rx[MQ_RX_BUF + 8];
static uint16_t s_rx_len;

static uint8_t   s_tx[MQ_TX_BUF];

static mq_state_t s_state = MQS_IDLE;
static uint32_t   s_tick;       /* 当前状态的起始时刻，用来算超时 */
static uint32_t   s_ping_tick;  /* 上次发心跳的时刻 */
static uint32_t   s_alive_tick; /* 上次收到任何合法报文的时刻，用来判死 */
static uint16_t   s_pkt_id;     /* 报文标识符，SUBSCRIBE/PUBLISH(QoS1) 共用，必须递增 */

/* ---------- 设备侧状态（上报用，也会被下行控制改写） ----------
   这就是「设备现在是什么样」。上行时整包发出去，下行命中的字段就地改，
   改完落到 GPIO —— 流程是：采集/下发 → s_status → GPIO + 上报。
   接传感器以后，把采集到的值写进 MQTT_Status() 返回的结构体即可。

   fan_status 初值 0 而 rfid 给个初值：rfid 空字符串上报出去网页端
   会显示一行空白，看不出是"没刷卡"还是"程序没跑"，给个定值更直观。 */
static mqtt_status_t s_status = { .mode = 1, .fan_status = 0, .rfid = "A1B2C3" };

static uint32_t s_pub_tick;     /* 上次上报的时刻 */
static uint8_t  s_pub_first;    /* 1 = 上线后还没报过，第一圈立刻发一次 */

/* ---------- 缓冲操作 ---------- */

/**
  * 函    数：mq_drop —— 从队首丢掉 n 个字节
  * 注    意：丢完顺便把尾部补齐零，保证长度字段解码永远读得到终止字节。
  */
static void mq_drop(uint16_t n)
{
    if (n >= s_rx_len)
    {
        s_rx_len = 0;
    }
    else
    {
        s_rx_len -= n;
        memmove(s_rx, s_rx + n, s_rx_len);
    }
    memset(s_rx + s_rx_len, 0, sizeof(s_rx) - s_rx_len);
}

/**
  * 函    数：mq_rx_pump —— 把串口里现有的字节全部搬进解析缓冲
  * 注    意：非阻塞。有多少取多少，取不到就返回，绝不等。
  */
static void mq_rx_pump(void)
{
    uint16_t avail = Uart_Available(UART_AT);
    uint16_t space;

    if (avail == 0)
    {
        return;
    }

    space = (uint16_t)(MQ_RX_BUF - s_rx_len);
    if (avail > space)
    {
        avail = space;
    }

    if (avail == 0)
    {
        /* 攒满 512 字节还拼不出一个完整报文 = 字节流已经彻底错位（多半是
           透传刚建立时混进了横幅）。整段丢弃重来，比一直卡死强。 */
        s_rx_len = 0;
        memset(s_rx, 0, sizeof(s_rx));
        return;
    }

    s_rx_len += Uart_Read(UART_AT, s_rx + s_rx_len, avail);
    memset(s_rx + s_rx_len, 0, sizeof(s_rx) - s_rx_len);
}

/**
  * 函    数：mq_pkt_len —— 判断队首是否已经攒够一个完整报文
  * 返 回 值：>0 = 整包字节数（头 + 长度字段 + 剩余长度）；0 = 还没收全；-1 = 长度非法
  */
static int mq_pkt_len(void)
{
    int rem  = 0;
    int used;

    if (s_rx_len < 2)
    {
        return 0;                       /* 连长度字段都还没开始 */
    }

    used = MQTTPacket_decodeBuf(s_rx + 1, &rem);
    if (used <= 0 || used > (int)s_rx_len - 1)
    {
        return 0;                       /* 长度字段本身还没收全（变长 1~4 字节） */
    }
    if (rem < 0 || rem > (MQ_RX_BUF - 5))
    {
        return -1;                      /* 长度离谱 = 已经错位 */
    }
    if ((1 + used + rem) > (int)s_rx_len)
    {
        return 0;                       /* 报文体还没到齐 */
    }
    return 1 + used + rem;
}

/**
  * 函    数：mq_type_ok —— 本状态愿不愿意处理这种报文
  * 注    意：这是「错位自愈」的关键。透传建立瞬间串口上可能残留 "\r\nOK\r\n"
  *           这类横幅尾巴，把它们当成报文头解析会得到离谱的长度，然后永远等不齐。
  *           所以只认本状态该出现的类型，其余一律逐字节丢弃。
  */
static uint8_t mq_type_ok(uint8_t type)
{
    switch (s_state)
    {
        case MQS_WAIT_CONNACK:  return (type == CONNACK) ? 1 : 0;
        case MQS_WAIT_SUBACK:   return (type == SUBACK) ? 1 : 0;
        case MQS_ONLINE:
            return (type == PUBLISH || type == PUBACK || type == SUBACK ||
                    type == PINGRESP) ? 1 : 0;
        default:                return 1;   /* IDLE/BACKOFF 期间收到的都先吃着，随后被清掉 */
    }
}

/* ---------- 发送 ---------- */

static void mq_send(const uint8_t *buf, int len)
{
    if (len > 0)
    {
        Uart_Write(UART_AT, buf, (uint16_t)len);
    }
}

/**
  * 函    数：mq_send_connect
  * 注    意：clientID / 用户名 / 密码都来自 mqtt_client.h 顶部那组宏，
  *           赛场服务器是普通用户名口令（admin / admin123），不是 token。
  *
  *           clientID 必须全服唯一：同名连接进来，broker 会把旧连接直接踢掉。
  *           所以在电脑上用 MQTTX 调试时，设备会被挤下线（反之亦然）——
  *           这不是故障，是 MQTT 规范行为。
  */
static void mq_send_connect(void)
{
    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    MQTTString cid  = MQTTString_initializer;
    MQTTString user = MQTTString_initializer;
    MQTTString pass = MQTTString_initializer;
    int len;

    cid.cstring  = MQTT_CLIENT_ID;
    user.cstring = MQTT_USERNAME;
    pass.cstring = MQTT_PASSWORD;

    data.MQTTVersion       = 4;                  /* 3.1.1 */
    data.clientID          = cid;
    data.username          = user;
    data.password          = pass;
    data.keepAliveInterval = MQTT_KEEPALIVE_S;
    data.cleansession      = 1;                  /* 每次重连都要一个干净会话 */
    data.willFlag          = 0;

    len = MQTTSerialize_connect(s_tx, sizeof(s_tx), &data);
    if (len <= 0)
    {
        printf("CONNECT 序列化失败（缓冲区不够）\r\n");
        return;
    }
    mq_send(s_tx, len);
    printf("→ CONNECT 已发出（%d 字节），等 CONNACK\r\n", len);
}

/**
  * 函    数：mq_send_subscribe
  * 注    意：同时订 Client_team_1_1 和 Clinet_team_1_1。
  *           任务书原文写的是 "Clinet"，MQTTX 里用的是 "Client"，
  *           目前无法确定哪个是对的，所以两个都订 —— 订错主题的症状是
  *           「发送正常、永远收不到控制指令」，这类 bug 最难往拼写上想。
  *
  *           上行主题 4G_team_1_1 不订：那是设备自己往外发的，
  *           订了只会收到自己刚发的那条 JSON，再被下行处理当成控制指令
  *           执行一遍（比如 fan 字段被自己"控制"一次），属于自激。
  */
static void mq_send_subscribe(void)
{
    MQTTString topics[2] = { MQTTString_initializer, MQTTString_initializer };
    int qos[2] = { 0, 0 };
    int len;

    topics[0].cstring = MQTT_TOPIC_SET;
    topics[1].cstring = MQTT_TOPIC_SET_ALT;

    len = MQTTSerialize_subscribe(s_tx, sizeof(s_tx), 0, ++s_pkt_id, 2, topics, qos);
    if (len <= 0)
    {
        printf("SUBSCRIBE 序列化失败\r\n");
        return;
    }
    mq_send(s_tx, len);
    printf("→ SUBSCRIBE 已发出（%d 字节）：%s + %s，等 SUBACK\r\n",
           len, MQTT_TOPIC_SET, MQTT_TOPIC_SET_ALT);
}

static void mq_send_pingreq(void)
{
    int len = MQTTSerialize_pingreq(s_tx, sizeof(s_tx));
    if (len > 0)
    {
        mq_send(s_tx, len);
    }
}

/* ---------- 上报 ---------- */

/**
  * 函    数：MQTT_Publish_Text —— 把现成字符串发到上行主题
  * 返 回 值：1 = 已发出，0 = 未发出（不在线 / 装不下）
  * 注    意：QoS 用 0。周期上报丢了下一包就补上；用 QoS1 要维护
  *           packet id 与 PUBACK 的匹配，对这个场景收益不值。
  *           比赛中 web 端是"显示当前值"，丢一包不影响理解。
  */
uint8_t MQTT_Publish_Text(const char *payload, uint16_t len)
{
    int         total;
    MQTTString  topic = MQTTString_initializer;

    if (s_state != MQS_ONLINE || payload == NULL || len == 0)
    {
        return 0;
    }

    topic.cstring = MQTT_TOPIC_POST;
    total = MQTTSerialize_publish(s_tx, sizeof(s_tx), 0, 0, 0, 0, topic,
                                  (unsigned char *)payload, (int)len);

    if (total <= 0)
    {
        /* 真发生时说明负载变长了而 MQ_TX_BUF 没跟着调，日志里能直接看出来 */
        printf("✗ 上报序列化失败：负载 %u 字节，TX 缓冲 %d 字节\r\n",
               (unsigned)len, (int)MQ_TX_BUF);
        return 0;
    }

    mq_send(s_tx, total);
    printf("↑ 上报 %d 字节：%.*s\r\n", total, (int)len, payload);
    return 1;
}

/**
  * 函    数：mq_tri —— 三角波，0 → span → 0，周期 2*span 秒
  * 注    意：用它而不是正弦，是因为不依赖 math.h 和 libm 的浮点函数，
  *           几行整数运算就够，且变化是线性的（网页上图线好看，不像随机跳）。
  */
#if MQ_SIM_DATA
static uint32_t mq_tri(uint32_t t, uint32_t span)
{
    uint32_t ph = t % (span * 2U);
    return (ph < span) ? ph : (span * 2U - ph);
}

/**
  * 函    数：mq_simulate —— 还没接传感器时，造一组会动的合理数据
  * 注    意：所有值都按「开机时长」算，不是随机数 —— 同样的时刻跑两次结果一样，
  *           调试时不会出现"刚才还能复现、重烧一次就变了"。
  *           精度刻意做成整十倍数再除，避免 cJSON 打出 25.600000000000001 这种尾巴。
  */
static void mq_simulate(void)
{
    uint32_t t = Board_Millis() / 1000U;     /* 开机秒数 */

    /* 温度 24.0~27.0℃，60 秒升 3℃，再 60 秒降回来 */
    s_status.temp  = (240.0 + (double)(mq_tri(t, 60U) * 30U) / 60.0) / 10.0;

    /* 湿度 55~70%，与温度反向走（真实现场大致如此，看起来更像那么回事） */
    s_status.humi  = (double)(55U + mq_tri(t, 45U) * 15U / 45U);

    /* 光照 800~1400lx */
    s_status.light = (double)(800U + mq_tri(t, 90U) * 600U / 90U);

    /* 土壤温度 28~37℃，上报时转成字符串 */
    s_status.solitemp = 28 + (int32_t)(mq_tri(t, 100U) * 9U / 100U);

    /* 两路旋钮 0.0~3.3V，相位错开，网页上两根曲线不会重叠 */
    s_status.knob1 = (double)(mq_tri(t, 30U) * 33U / 30U) / 10.0;
    s_status.knob2 = (double)(mq_tri(t + 17U, 40U) * 33U / 40U) / 10.0;

    /* rfid 不动：本工程没有读卡器，它代表「当前这张卡」，
       刷卡时才该变。要测网页端列表，直接改 MQTT_Status()->rfid 即可。 */
}
#endif

/* 上行 JSON 的最大长度。实测 145 字节左右（rfid 满 15 字符时也够），
   留一倍余量便于以后加字段；超了会在日志里报出来，不会静默截断。 */
#define MQ_PAYLOAD_MAX  192

/**
  * 函    数：mq_num —— 把浮点数写成「1 位或 2 位小数」的文本
  * 参    数：v —— 值；dec —— 小数位数
  * 注    意：这里不用 "%.1f"，也不用 cJSON 的自动格式化，原因是：
  *
  *           cJSON 打完数字会用 sscanf("%lg") 回读校验，而本工程只链了
  *           -u _printf_float（printf 能打浮点），没链 -u _scanf_float。
  *           nano 库里 sscanf 遇到 %lg 直接返回 0，判成「还原失败」，
  *           于是回退到 %1.17g —— 线上就出现 24.399999999999999 这种尾巴。
  *           实测就是这么来的：值本身是 24.4，尾巴纯属格式化产物。
  *
  *           所以先把值放大成整数刻度（四舍五入），再用 %ld 拼小数点，
  *           全程不碰浮点格式化函数，输出和样例逐字一致，也不依赖
  *           链接选项（以后谁删了 _printf_float 也不会变）。
  */
static void mq_num(char *dst, size_t n, double v, uint8_t dec)
{
    int32_t scale = (dec == 2U) ? 100 : (dec == 1U) ? 10 : 1;
    long    s     = (long)((v < 0.0) ? (v * scale - 0.5) : (v * scale + 0.5));
    const char *sign = (s < 0) ? "-" : "";
    long    a     = ((s < 0) ? -s : s) / scale;   /* 整数部分 */
    long    b     = ((s < 0) ? -s : s) % scale;   /* 小数部分 */

    if (dec == 0U)
    {
        snprintf(dst, n, "%s%ld", sign, a);
    }
    else if (dec == 1U)
    {
        snprintf(dst, n, "%s%ld.%01ld", sign, a, b);
    }
    else
    {
        snprintf(dst, n, "%s%ld.%02ld", sign, a, b);
    }
}

/**
  * 函    数：mq_publish_report —— 把当前状态打成一行 JSON 发出去
  * 注    意：手写而不是走 cJSON_Print，是为了让键盘上的格式和线上完全一致
  *           （数字精度、字段顺序、空格都固定）。下行解析仍然用 cJSON。
  *
  *           字段类型不能含糊：
  *             light        不是 lux（改了网页端就取不到）
  *             solitemp     是字符串 "32"，不是数字 32
  *             fan_status   不是 fan
  *             rfid         字符串
  */
static void mq_publish_report(void)
{
    char payload[MQ_PAYLOAD_MAX];
    char temp[16];
    char k1[16];
    char k2[16];
    int  len;

    mq_num(temp, sizeof(temp), s_status.temp,  1U);
    mq_num(k1,   sizeof(k1),   s_status.knob1, 1U);
    mq_num(k2,   sizeof(k2),   s_status.knob2, 1U);

    len = snprintf(payload, sizeof(payload),
                   "{\"temp\": %s, \"humi\": %d, \"light\": %ld, "
                   "\"solitemp\": \"%d\", \"knob1\": %s, \"knob2\": %s, "
                   "\"mode\": %d, \"fan_status\": %d, \"rfid\": \"%s\"}",
                   temp,
                   (int)s_status.humi,
                   (long)s_status.light,
                   (int)s_status.solitemp,
                   k1, k2,
                   (int)s_status.mode,
                   (int)s_status.fan_status,
                   s_status.rfid);

    if (len <= 0 || len >= (int)sizeof(payload))
    {
        printf("✗ JSON 拼装越界：需要 %d 字节，缓冲 %d 字节\r\n",
               len, (int)sizeof(payload));
        return;
    }

    (void)MQTT_Publish_Text(payload, (uint16_t)len);
}

/**
  * 函    数：MQTT_Status —— 交出设备状态结构体的指针
  * 返 回 值：可直接读写的状态
  * 注    意：业务层采集到数据后写这里，下一轮上报就自动带上；
  *           改完想立刻生效，调 mq_apply_outputs()（内部私有，见 mq_set_field）。
  */
mqtt_status_t *MQTT_Status(void)
{
    return &s_status;
}

/* ---------- 下行 ---------- */

/* 红灯翻转节拍。放在文件作用域而不是函数内 static，
   是因为 mq_apply_outputs() 在「启动」那一刻要把它清零，
   否则上一轮留下的旧时间戳会让灯一亮就立刻被翻灭。 */
static uint32_t s_led_tick;

/**
  * 函    数：mq_apply_outputs —— 把状态落到 GPIO
  * 注    意：本板用一颗 LED 代表风扇状态（引脚和极性都在 board.c 里，这里不关心）。
  *           赛题原文要求「按启动后风扇转、蜂鸣器响、红灯 1Hz 闪烁」，
  *           所以这里只负责「启动瞬间点亮 / 停止瞬间熄灭」，
  *           1Hz 的翻转交给 mq_output_tick()。
  *           真接上风扇/继电器/蜂鸣器时，在这里加一行就行 —— 所有输出都从
  *           这里出去，不要在别处直接写 GPIO，否则状态和硬件会慢慢对不上。
  */
static void mq_apply_outputs(void)
{
    if (s_status.fan_status != 0)
    {
        Led_Fan(1);                       /* 先亮 */
        Timer_Reset(&s_led_tick);         /* 下一次翻转从现在重新计 500ms */
    }
    else
    {
        /* 停止要「立即」，不能等下一个 500ms 的节拍 */
        Led_Fan(0);
    }
}

/**
  * 函    数：mq_output_tick —— 风扇运转期间的 1Hz 闪烁
  * 注    意：非阻塞，主循环里每圈调一次。用「距上次翻转是否满 500ms」
  *           而不是 HAL_Delay，否则整个 MQTT 收发和心跳都会被卡住
  *           （心跳一停 broker 就踢人，表现为「灯闪得挺好、5 分钟后掉线」）。
  *           fan_status=0 时直接返回，灭灯已经由 mq_apply_outputs 做过。
  */
static void mq_output_tick(void)
{
    if (s_status.fan_status == 0U)
    {
        return;
    }
    if (Timer_Expired(&s_led_tick, 500U))
    {
        Led_Fan_Toggle();
    }
}

/**
  * 函    数：mq_set_field —— 按字段名改一个状态量
  * 返 回 值：1 = 认得这个字段，0 = 不认识
  * 注    意：字段名必须和上报 JSON 里的键一致（fan_status / mode），
  *           网页端按下按钮发 {"fan_status":1} 就走到这里。
  *
  *           "fan" 是老写法，留作别名：网页端如果早先按旧格式写好，
  *           不改也能控制，省得为了一个键名回头改前端。
  */
static uint8_t mq_set_field(const char *key, double v)
{
    if (key == NULL)
    {
        return 0;
    }

    if (strcmp(key, "fan_status") == 0 || strcmp(key, "fan") == 0)
    {
        s_status.fan_status = (v != 0.0) ? 1 : 0;
        mq_apply_outputs();
        printf("  ↳ 风扇 %s\r\n", (s_status.fan_status != 0) ? "启动" : "停止");
        return 1;
    }
    if (strcmp(key, "mode") == 0)
    {
        s_status.mode = (v != 0.0) ? 1 : 0;
        printf("  ↳ 模式 %s\r\n", (s_status.mode != 0) ? "自动" : "手动");
        return 1;
    }

    printf("  ↳ 未知字段 %s，忽略\r\n", key);
    return 0;
}

/**
  * 函    数：mq_handle_downlink —— 解析控制指令并落到设备上
  * 参    数：payload / len —— 报文负载（不是 C 字符串，可能不含 '\0'）
  * 注    意：必须用 cJSON_ParseWithLength。透传通道上的负载没有结尾零，
  *           用 cJSON_Parse 会一直往后读到缓冲里的残渣，行为不可预测。
  *
  *           比赛服务器是裸 broker，没有 OneNET 那套 set/reply 回执机制，
  *           所以这里只管改状态，不回任何东西。
  */
static void mq_handle_downlink(const char *payload, int len)
{
    cJSON  *root;
    cJSON  *item;
    uint8_t changed = 0;

    if (payload == NULL || len <= 0)
    {
        return;
    }

    root = cJSON_ParseWithLength(payload, (size_t)len);
    if (root == NULL)
    {
        printf("✗ 下行 JSON 解析失败，原文见上一行\r\n");
        return;
    }

    cJSON_ArrayForEach(item, root)
    {
        double v;

        if (item->string == NULL)
        {
            continue;
        }
        if (!cJSON_IsNumber(item))
        {
            continue;               /* 只认数值型控制字段 */
        }

        v = item->valuedouble;
        if (mq_set_field(item->string, v))
        {
            changed = 1;
        }
    }

    cJSON_Delete(root);

    /* 改完立刻回传一次，网页上不用等下一个上报周期就能看到变化 */
    if (changed && MQ_AUTO_REPORT)
    {
        mq_publish_report();
        s_pub_tick = Board_Millis();
    }
}

/* ---------- 状态流转 ---------- */

static void mq_backoff(void)
{
    s_state = MQS_BACKOFF;
    s_tick  = Board_Millis();
    s_rx_len = 0;
    memset(s_rx, 0, sizeof(s_rx));
}

static void mq_start(void)
{
    s_rx_len = 0;
    memset(s_rx, 0, sizeof(s_rx));
    s_tick   = Board_Millis();
    s_state  = MQS_WAIT_CONNACK;    /* 先立状态再发，回包永远在下一轮才可能被处理 */

    /* 让 GPIO 立刻和状态对齐。gpio.c 里 PC13 初始化成低电平（=灯亮），
       而 s_status.fan_status 初值是 0，不对齐的话上电后灯亮着但上报说风扇停 —— 
       一个"看得见的谎"，排查时最费时间。 */
    mq_apply_outputs();

    mq_send_connect();
}

/* ---------- 解析 ---------- */

static void mq_handle_publish(int total)
{
    unsigned char  dup = 0, retained = 0;
    int            qos = 0;
    unsigned short pkt_id = 0;
    MQTTString     topic = MQTTString_initializer;
    unsigned char *payload = NULL;
    int            payload_len = 0;
    char           name[64];

    if (MQTTDeserialize_publish(&dup, &qos, &retained, &pkt_id, &topic,
                                &payload, &payload_len, s_rx, total) != 1)
    {
        return;
    }

    /* 主题是 MQTTString，可能是 cstring 也可能是带长度的 lenstring，两种都要认 */
    if (topic.cstring != NULL)
    {
        snprintf(name, sizeof(name), "%s", topic.cstring);
    }
    else
    {
        int n = topic.lenstring.len;
        if (n > (int)sizeof(name) - 1) n = (int)sizeof(name) - 1;
        memcpy(name, topic.lenstring.data, (size_t)n);
        name[n] = '\0';
    }

    printf("← 收到下行 [%s] %d 字节：%.*s\r\n", name, payload_len, payload_len, (char *)payload);

    /* QoS1 必须回 PUBACK，否则 broker 会一直重发这条 */
    if (qos == 1)
    {
        int len = MQTTSerialize_puback(s_tx, sizeof(s_tx), pkt_id);
        if (len > 0)
        {
            mq_send(s_tx, len);
        }
    }

    /* 两个拼写都算下行控制指令。
       任务书原文是 Clinet_team_1_1，MQTTX 里是 Client_team_1_1，
       订阅时两个都订了，这里也必须两个都认 —— 否则订上了却不处理，
       现象和没订一模一样（日志里能看到收到报文，但灯不动作）。

       上行主题没订，所以这里不会收到自己发出去的那条，
       不存在「自己的上报被自己当控制指令执行一遍」的自激。 */
    if (strcmp(name, MQTT_TOPIC_SET) == 0 || strcmp(name, MQTT_TOPIC_SET_ALT) == 0)
    {
        mq_handle_downlink((const char *)payload, payload_len);
    }
}

static void mq_parse(void)
{
    int     total;
    uint8_t type;

    /* 第一关：队首字节必须是本状态认识的类型，否则逐字节丢 */
    while (s_rx_len > 0 && !mq_type_ok((uint8_t)(s_rx[0] >> 4)))
    {
        mq_drop(1);
    }
    if (s_rx_len == 0)
    {
        return;
    }

    total = mq_pkt_len();
    if (total < 0)
    {
        mq_drop(1);             /* 长度非法，假定错位一字节，继续对齐 */
        return;
    }
    if (total == 0)
    {
        return;                 /* 还没收全，下次再说 */
    }

    type = (uint8_t)(s_rx[0] >> 4);

    /* 收到一条完整报文 = 链路还活着。不管是 CONNACK、SUBACK、PINGRESP
       还是下行 PUBLISH，都算「对面还在」。放这里统一喂，省得每个 case
       都写一遍，也避免了「以后新增报文类型忘了喂狗」这种低级漏判。 */
    s_alive_tick = Board_Millis();

    switch (type)
    {
        case CONNACK:
            if (total >= 4)
            {
                uint8_t rc = s_rx[3];
                if (rc == 0)
                {
                    printf("✓ CONNACK：服务器已接受连接\r\n");
                    s_state = MQS_WAIT_SUBACK;
                    s_tick  = Board_Millis();
                    mq_send_subscribe();
                }
                else
                {
                    /* 常见的 4 = 用户名/密码错，5 = 未授权。
                       出现这两个，先去核对 mqtt_client.h 里的
                       MQTT_USERNAME / MQTT_PASSWORD（赛场是 admin / admin123），
                       别再盲目重连 —— 口令错的时候重连一百次也是错。 */
                    printf("✗ CONNACK 被拒绝，返回码 %u（4=账号密码错 5=未授权）\r\n", rc);
                    mq_backoff();
                }
            }
            break;

        case SUBACK:
            if (total >= 6)
            {
                /* SUBACK 结构：报文头 2 字节 + packet id 2 字节 + 每个主题一个返回码。
                   返回码 0x00/0x01/0x02 = 授予的 QoS，0x80 = 订阅失败
                   （不是 0xFF —— 按 0xFF 判会把真正的失败整个漏掉）。
                   两个主题各有一个码：一个成一个败说明拼写只对了一半，
                   日志里能当场看出来，不用猜。 */
                uint8_t granted = 0;
                int     i;

                for (i = 4; i < total && i < 6; i++)
                {
                    if (s_rx[i] >= 0x80)
                    {
                        printf("✗ 主题 %s 被拒绝（返回码 0x%02X）\r\n",
                               (i == 4) ? MQTT_TOPIC_SET : MQTT_TOPIC_SET_ALT,
                               s_rx[i]);
                    }
                    else
                    {
                        granted++;
                    }
                }

                if (granted > 0)
                {
                    s_state     = MQS_ONLINE;
                    s_ping_tick = Board_Millis();
                    s_alive_tick = s_ping_tick;   /* 判活计时从上线这一刻起算 */
                    s_pub_first = 1;        /* 上线第一圈就报一次，别让人干等 5 秒 */
                    printf("✓ 订阅成功（%u/2 个主题），设备在线\r\n", (unsigned)granted);
                }
                else
                {
                    mq_backoff();
                }
            }
            break;

        case PUBLISH:
            mq_handle_publish(total);
            break;

        case PINGRESP:
            /* 心跳有回音。s_alive_tick 已经在 switch 之前统一喂过了，
               这里不用再做事 —— 判活在 MQS_ONLINE 里按静默时长做。 */
            break;

        default:
            break;
    }

    /* 解析过程中若已转入 BACKOFF，缓冲被清空，这里就不能再按旧长度丢 */
    if (s_state != MQS_BACKOFF && s_state != MQS_IDLE)
    {
        mq_drop((uint16_t)total);
    }
}

/* ---------- 对外接口 ---------- */

/**
  * 函    数：MQTT_Client_Loop —— 非阻塞推进 MQTT 状态机
  * 参    数：无
  * 返 回 值：无
  * 注    意：主循环里每次转一圈调一次。通道没就绪时立刻返回，不影响配网。
  */
void MQTT_Client_Loop(void)
{
    /* 放在最前面：闪灯与网络无关，没联网时也不能漏掉节拍 */
    mq_output_tick();

    if (!ESP8266_IsTransparent())
    {
        /* 通道没了（还没配好网，或刚刚被 ESP8266_ExitTransparent 打回 AT）。
           此时 MQTT 层的会话一律作废：旧连接已经不存在，还留着 WAIT_CONNACK
           之类的状态，等通道恢复后会拿着上次的超时时刻去判，行为很别扭。
           清成 IDLE，通道一恢复就从 CONNECT 老老实实重来。 */
        if (s_state != MQS_IDLE)
        {
            MQTT_Client_Reset();
        }
        return;
    }

    if (s_state == MQS_IDLE)
    {
        mq_start();
    }

    mq_rx_pump();
    mq_parse();

    switch (s_state)
    {
        case MQS_WAIT_CONNACK:
            if ((Board_Millis() - s_tick) >= MQ_CONNACK_MS)
            {
                printf("✗ 等 CONNACK 超时（%ums）\r\n", (unsigned)MQ_CONNACK_MS);
                mq_backoff();
            }
            break;

        case MQS_WAIT_SUBACK:
            if ((Board_Millis() - s_tick) >= MQ_SUBACK_MS)
            {
                printf("✗ 等 SUBACK 超时（%ums）\r\n", (unsigned)MQ_SUBACK_MS);
                mq_backoff();
            }
            break;

        case MQS_ONLINE:
            /* 判死：一段时间内一个合法报文都没收到，说明 TCP 那头已经不是
               我们以为的那个连接了（AP 掉了 / 服务器重启 / 模块自己断开）。

               这一条以前是缺的，后果很隐蔽：设备照样每 30 秒发 PINGREQ，
               但发出去就掉进黑洞，永远等不到 PINGRESP；而状态机停在 ONLINE
               不动，网页显示"设备在线"却一个数据都没有，OLED 也跟着显示
               ONLINE。现场排查时看到"在线但没数据"，第一反应会去查主题名，
               其实是通道早死了。现在宁可多断一次，也不能骗人。 */
            if ((Board_Millis() - s_alive_tick) >= MQ_DEAD_MS)
            {
                printf("✗ %u 秒没收到服务器任何报文，判定掉线，重建连接\r\n",
                       (unsigned)(MQ_DEAD_MS / 1000U));

                MQTT_Client_Reset();        /* MQTT 会话作废 */
                ESP8266_ExitTransparent();  /* 连 TCP 一起重建，别在原连接上重试 */
                break;
            }

            /* 半个 keepalive 周期发一次心跳。不靠 broker 的 1.5 倍周期硬撑，
               因为 TCP 可能早就死了而本地毫无察觉（透传通道没有断线中断）。 */
            if ((Board_Millis() - s_ping_tick) >= MQ_PING_MS)
            {
                s_ping_tick = Board_Millis();
                mq_send_pingreq();
            }

            /* 周期上报。放在心跳之后：心跳是保命，上报是业务，
               万一这轮时隙不够，先保证连接不被踢掉。
               MQ_SIM_DATA=1 时先刷一遍模拟值，让网页端每 5 秒都能看到数据在动。 */
            if (MQ_AUTO_REPORT &&
                (s_pub_first || (Board_Millis() - s_pub_tick) >= MQ_PUB_MS))
            {
                s_pub_first = 0;
                s_pub_tick  = Board_Millis();
#if MQ_SIM_DATA
                mq_simulate();
#endif
                mq_publish_report();
            }
            break;

        case MQS_BACKOFF:
            if ((Board_Millis() - s_tick) >= MQ_BACKOFF_MS)
            {
                s_state = MQS_IDLE;     /* 下一轮从 CONNECT 重新来 */
            }
            break;

        case MQS_IDLE:
        default:
            break;
    }
}

/**
  * 函    数：MQTT_IsOnline —— 设备是否真的在线
  * 返 回 值：1 = 已连上且已订阅，可以发上行数据
  */
uint8_t MQTT_IsOnline(void)
{
    return (s_state == MQS_ONLINE) ? 1 : 0;
}

/**
  * 函    数：MQTT_Client_Reset —— 主动丢弃当前会话
  * 注    意：留给以后「检测到掉线」时调用。透传通道一旦断了，
  *           模块自己会重连（CWAUTOCONN），但 MQTT 会话必须重新握手。
  */
void MQTT_Client_Reset(void)
{
    s_rx_len = 0;
    memset(s_rx, 0, sizeof(s_rx));
    s_state  = MQS_IDLE;
}
