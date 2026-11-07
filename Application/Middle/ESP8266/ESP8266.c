/* ============================================================================
 * 文件：ESP8266.c
 * 作用：ESP-01S（ESP8266）WiFi 模块的 AT 指令配网 + 进入透传
 *
 * 硬件：USART2 <-> ESP-01S，接收走 DMA 环形缓冲（见 BSP/UART/uart_app.c）
 * 调用：main 的 while(1) 里反复调 ESP8266_Loop()，每次只推进一步、立即返回
 *
 * 整体流程（表驱动，指令表见下面的 k_net_steps）：
 *
 *   NET_SEND ──发指令──> NET_WAIT ──凑出完整应答──> 下一条 / 失败重试
 *                          │
 *                          └── 超时 ──> 重发当前这条
 *
 *   表走完（AT+CIPSEND 收到 ">"）──> NET_TRANSPARENT
 *                                    （通道归上层，本文件不再碰 AT）
 *
 * 为什么用 CIPMODE + CIPSTART + CIPSEND，而不是 SAVETRANSLINK + AT+RST：
 *   SAVETRANSLINK 的做法是「把服务器地址写进模块 Flash → 复位 → 让模块自己连」。
 *   它有两个致命问题：
 *     1) 复位后能不能连上，取决于模块自己的自动重连，我们只能干等。实测模块
 *        复位后只吐一句 ready 就再无下文（既没有 WIFI CONNECTED 也没有 GOT IP），
 *        而 AT+CWJAP 明明是回 OK 的 —— 因为 CWJAP 只对本次运行生效，
 *        复位后模块去找的是 Flash 里那条旧热点。
 *     2) 它把「连上了没有」变成一个只能靠猜的过程。以前固定等 4 秒就宣布
 *        进透传，结果是通道根本没通，上层发出去的 MQTT 报文全掉进虚空。
 *   现在这条路全程有应答：CIPSTART 成功回 CONNECT、失败回 ERROR/CLOSED，
 *   CIPSEND 回 ">" 才算真正能发数据。每一步都有判据，不用猜。
 *   附带好处：不再复位，省掉 10~20 秒的重启重连时间。
 *
 * 为什么不做成「发一条、死等一条」：
 *   AT+CWJAP 连路由器要 2~6 秒，阻塞等待会把整个主循环卡死，别的任务全停。
 *   这里把等待拆成「每次进来查一次」，主循环转多少圈都不影响功能。
 * ========================================================================== */

#include "ESP8266.h"
#include <string.h>
#include "stdbool.h"
#include "uart_app.h"
#include "board.h"              /* Board_Millis()：超时判断，不直接用 HAL_GetTick */

/* ==================== 配网指令表 ====================
   把变化的部分抽成表（指令 / 超时 / 失败时补发什么 / 判据），流程只写一遍：
       NET_SEND 发当前这条 → NET_WAIT 等应答 → 下一条，或失败重试
   以后要加指令，只在表里加一行，switch 不用动。
   ==================================================== */

/* 同一条指令连续失败几次后，放弃局部重试，从第 0 条整体重来。
   取 3 是经验值：单次失败多由模块忙、回显错位引起，重试通常能自愈；
   连错 3 次基本可以判定链路已经乱了，这时候从 AT 重新握手比继续死磕有用。 */
#define STEP_RETRY_MAX      3U

/* 表里第 0 步 = AT 探测。它超时说明模块可能停在透传模式（压根不理 AT），
   这时候要发 +++ 把它拽出来。 */
#define STEP_AT_PROBE       0U

typedef enum {//状态机的状态
    NET_SEND = 0,               /* 发送当前步骤的指令 */
    NET_WAIT,                   /* 等模块应答 */
    NET_TRANSPARENT,            /* 透传通道就绪，本文件不再碰 AT */
} net_state_t;

typedef struct { //发送的数据结构体定义
    const char *cmd;            /* 要发送的 AT 指令；含 %s 时由 arg 填充 */
    uint16_t    timeout_ms;     /* 从发出到判定超时的毫秒数，最大 65535 */
    const char *on_fail;        /* 补发指令；NULL 表示「收到别的帧也不理它」 */
    const char *arg;            /* cmd 中 %s 的实参；无占位符时写 NULL */
    const char *reject;         /* 应答里出现这串就判失败；NULL = 只看 "OK" */
    uint8_t     retry_forever;  /* 1 = 失败后一直重发，不退回第 0 步；省略即 0 */
    const char *accept;         /* 应答里出现这串也算成功（如 CONNECT / >）；
                                   有些指令成功时不回 OK，必须单独指定 */
} net_step_t;

static const net_step_t k_net_steps[] = {
    /* 第 0 步：探 AT。超时就发 +++，把可能停在透传里的模块拽回 AT 模式。
       1.5 秒超时是给 +++ 前后各留够 1 秒静默。 */
    { AT_,              1500,  EXIT_PASS,     NULL,      NULL,       0, NULL },
    /* 清掉 Flash 里可能残留的透传配置。以前版本写过 SAVETRANSLINK=1，
       不清掉的话模块每次上电都会自动进透传，AT 探测又得来一遍 ++++++。 */
    { AT_TRANS_OFF,     1000,  AT_RST,        NULL,      NULL,       0, NULL },
    { AT_NET1,          1000,  AT_RST,        NULL,      NULL,       0, NULL },//STA 模式
    { AT_NET2,          1000,  AT_RST,        NULL,      NULL,       0, NULL },//上电自动连接
    /* 连热点。必须用 _DEF 版本（写 Flash）：否则一次掉电就得重新配，
       现场比赛没时间改代码重编译。15 秒是模块连 AP 的最坏耗时。 */
    { AT_NET3,         15000,  NULL,          NULL,      NULL,       0, NULL },
    /* 查有没有真的拿到 IP。STATUS:5 = 没连上 AP，判失败。 */
    { AT_CIPSTA,        2000,  NULL,          NULL,      "STATUS:5", 0, NULL },
    /* 开透传模式。这条只是设个标志，不建立连接。 */
    { AT_CIPMODE,       1000,  NULL,          NULL,      NULL,       0, NULL },
    /* 建 TCP 连接。成功回 "CONNECT"，失败回 "ERROR" 或 "CLOSED"。
       所以 accept 写 CONNECT —— 它成功时不保证带 OK。 */
    { AT_CIPSTART,     15000,  NULL,          MQTT_HOST, "ERROR",    0, "CONNECT" },
    /* 申请发送。回 ">" 之后 USART2 上跑的就是裸 TCP 字节流了。
       这一条是整个配网流程的终点。 */
    { AT_CIPSEND,       3000,  NULL,          NULL,      NULL,       0, ">" },
};
#define NET_STEP_COUNT      (sizeof(k_net_steps) / sizeof(k_net_steps[0]))

/* 表里几条关键指令的下标，给「重连快路径」用。
   注意：这几个数必须和上面 k_net_steps 的书写顺序一致，插行就要跟着改。
   之所以不写成枚举，是因为表本身是个匿名初始化列表，两者没法自动对齐；
   与其加一层容易忘的机制，不如把依赖关系写在注释里。 */
#define STEP_AT             0   /* 探 AT，能从透传里拽回 AT 模式 */
#define STEP_CIPMODE        6   /* AT+CIPMODE=1 */
#define STEP_CIPSTART       7   /* AT+CIPSTART，真正建 TCP */


/* ---------- 状态机自己的运行时变量 ----------
   全部 static：外部看不见，也不会污染全局符号表。 */

static net_state_t s_state = NET_SEND;  /* 当前处于哪个状态 */
static uint8_t     s_idx;       /* 走到表里第几条指令 */
static uint8_t     s_retry;     /* 当前这条指令已重试几次，收到 OK 就清零 */
static uint8_t     s_fail_used; /* 本次尝试是否已补发过 on_fail（0/1 标志） */
static uint32_t    s_tick;      /* 本条指令的发送时刻，用来算超时 */
static uint8_t     s_fast_reconnect; /* 1 = 这次是掉线重连，走快路径（见下） */

/* 带 %s 占位符的指令（表里 arg 非 NULL 的那几条）先在这里拼好再发。
   放 static 不放栈上：AT_Loop 已经有一个 128 字节的 chunk，
   再加 80 字节栈会让 2KB 的栈更吃紧；这块内容发完就没用了，不必保护。
   80 字节够放 "AT+CIPSTART=\"TCP\",\"mqtts.heclouds.com\",1883\r\n"（48 字节）。 */
static char s_cmd_buf[80];

/* 应答累积缓冲 —— 这是本文件最关键的改动。
   以前的判据是「一个 IDLE 帧 = 一次完整应答」，但 AT 模块的回包根本不是这个结构：
   AT+CIPSTATUS 可能回一帧 "STATUS:2\r\n\r\nOK"，也可能分成两帧；
   AT+CIPSTART 会先来 "CONNECT"，隔一会儿再来 "OK"。
   按帧判定就会出现「该成功的判失败、该失败的判成功」两种相反的错。
   现在把收到的每一帧都追加到同一个缓冲，等缓冲里出现终止标记（OK/ERROR/
   FAIL/该步的 accept）才做判断 —— 判断永远在「整条应答」上做。 */
static char     s_rsp[256];
static uint16_t s_rsp_len;

/**
  * 函    数：rsp_append —— 把新收到的一段应答并入累积缓冲
  * 参    数：data/len —— 本次从串口读到的字节
  * 注    意：缓冲满了丢最旧的。AT+CWJAP 的回显有三十多字节，加上横幅可能
  *           一次来两百多，保留最近的才能把关键的那句 OK 留住。
  */
static void rsp_append(const uint8_t *data, uint16_t len)
{
    if (len == 0)
    {
        return;
    }

    if ((uint16_t)(s_rsp_len + len) > sizeof(s_rsp) - 1)
    {
        uint16_t drop = (uint16_t)(s_rsp_len + len - (sizeof(s_rsp) - 1));

        if (drop >= s_rsp_len)
        {
            s_rsp_len = 0;                  /* 旧的整段都留不住了，直接清空 */
        }
        else
        {
            memmove(s_rsp, s_rsp + drop, s_rsp_len - drop);
            s_rsp_len = (uint16_t)(s_rsp_len - drop);
        }
        if (len > sizeof(s_rsp) - 1)
        {
            data += (len - (sizeof(s_rsp) - 1));    /* 新数据本身也超长，只留尾部 */
            len   = sizeof(s_rsp) - 1;
        }
    }

    memcpy(s_rsp + s_rsp_len, data, len);
    s_rsp_len = (uint16_t)(s_rsp_len + len);
    s_rsp[s_rsp_len] = '\0';
}

/**
  * 函    数：rsp_is_complete —— 这条应答算不算「说完了」
  * 注    意：看到终止标记就算说完，不必等超时。这样大部分指令几百毫秒内
  *           就能推进到下一步，整体配网从十几秒压到几秒。
  */
static uint8_t rsp_is_complete(const net_step_t *st)
{
    if (s_rsp_len == 0)
    {
        return 0;
    }
    if (st->accept != NULL && strstr(s_rsp, st->accept) != NULL)
    {
        return 1;
    }
    if (strstr(s_rsp, "OK") != NULL || strstr(s_rsp, "ERROR") != NULL ||
        strstr(s_rsp, "FAIL") != NULL)
    {
        return 1;
    }
    /* CLOSED 只在「等连接建立」的步骤里当终止标记。
       别的步骤里它多半是上一条指令（比如 +++）迟到的回包，
       拿它当失败依据会冤枉当前这条指令。 */
    if (st->accept != NULL && strstr(s_rsp, "CLOSED") != NULL)
    {
        return 1;
    }
    return 0;
}

/**
  * 函    数：rsp_is_ok —— 在整条应答上判断成功与否
  */
static uint8_t rsp_is_ok(const net_step_t *st)
{
    if (st->reject != NULL && strstr(s_rsp, st->reject) != NULL)
    {
        return 0;                           /* 出现「明确表示不对」的内容，一票否决 */
    }
    if (st->accept != NULL && strstr(s_rsp, st->accept) != NULL)
    {
        return 1;
    }
    return (strstr(s_rsp, "OK") != NULL) ? 1 : 0;
}

/**
  * 函    数：step_failed —— 当前这条指令失败后的统一处理
  * 注    意：优先级是「先补发纠偏指令，再谈重试」。
  *           为什么要 s_fail_used 这个标志：AT+RST 会让模块重启并连吐横幅，
  *           横幅里没有期望的字符串，不加限制就会每收到一行就再补发一次，
  *           形成自激循环。所以规定：同一次尝试里 on_fail 最多补发一次。
  */
static void step_failed(void)
{
    s_rsp_len = 0;
    s_rsp[0]  = '\0';

    if (k_net_steps[s_idx].on_fail != NULL && s_fail_used == 0)
    {
        Uart_Printf(UART_AT, k_net_steps[s_idx].on_fail);
        s_tick      = Board_Millis();        /* 补发后重新计时 */
        s_fail_used = 1;
        return;                             /* 原地再等一轮，不重发原指令 */
    }

    /* 重试次数用完，判定这条链路整体不可信，退回第一条重新握手。
       retry_forever 的行跳过这段：一直重发当前这条，永远不退回。 */
    if (k_net_steps[s_idx].retry_forever == 0 && ++s_retry >= STEP_RETRY_MAX)
    {
        s_idx   = 0;
        s_retry = 0;
    }
    s_state = NET_SEND;
}

/**
  * 函    数：AT_Loop —— 配网状态机走一步
  * 参    数：无
  * 返 回 值：无
  * 注    意：非阻塞。每次调用只做当前状态该做的那一件事，然后立刻返回。
  *           由 ESP8266_Loop() 包装，供 main 的 while(1) 循环调用。
  */
static void AT_Loop(void)
{
    uint8_t  chunk[128];
    uint16_t n;

    switch (s_state)
    {
        /* ---------- 状态一：发指令 ---------- */
        case NET_SEND:
            if (s_idx == 0)
            {
                Uart_FlushRx(UART_AT);  /* 只把读指针追平写指针，不动缓冲内容 */
            }

            s_rsp_len = 0;              /* 新一轮问答，应答缓冲清空 */
            s_rsp[0]  = '\0';

            //当指令需要填充参数时
            if (k_net_steps[s_idx].arg != NULL)
            {
                snprintf(s_cmd_buf, sizeof(s_cmd_buf),
                         k_net_steps[s_idx].cmd, k_net_steps[s_idx].arg);//拼接参数
                Uart_Printf(UART_AT, "%s", s_cmd_buf);//发送
            }
            else
            {
                Uart_Printf(UART_AT, k_net_steps[s_idx].cmd);
            }

            s_tick      = Board_Millis();    /* 记下发送时刻，超时从这里算 */
            s_fail_used = 0;                /* 新的一次尝试，补发机会重新给一次 */
            s_state     = NET_WAIT;         /* 发完立刻转去等，不停留 */
            break;

        /* ---------- 状态二：等回应 ---------- */
        case NET_WAIT:
            /* Uart_FrameReady 只读不清：返回 1 表示串口上出现了一次 IDLE 事件，
               也就是「一帧数据收完了」。它不消费数据，真正的清除发生在
               Uart_Read 里，所以「看到有帧」和「取走帧」之间不会丢数据。 */
            if (Uart_FrameReady(UART_AT))
            {
                n = Uart_Read(UART_AT, chunk, sizeof(chunk) - 1);
                rsp_append(chunk, n);       /* 可能是半条应答，先攒着 */
            }

            /* 攒够一整条应答才判。判完立刻清空，等下一问。 */
            if (rsp_is_complete(&k_net_steps[s_idx]))
            {
                if (rsp_is_ok(&k_net_steps[s_idx]))
                {
                    s_rsp_len = 0;
                    s_rsp[0]  = '\0';
                    s_retry   = 0;          /* 这一步过了，重试计数清零 */

                    /* 重连快路径：AT 探测能通，说明模块还在、AT 模式进得来。
                       此时 WiFi 参数早就写在 Flash 里了，没必要再发一遍
                       CWMODE / CWAUTOCONN / CWJAP_DEF —— 尤其 CWJAP_DEF 是写
                       Flash 的操作，掉线一次写一次，白白消耗擦写寿命，
                       而且连 AP 本身还要花两三秒。直接去重建 TCP 更快。

                       万一 AP 真的也掉了，CIPSTART 会回 ERROR，
                       step_failed 把 s_idx 退回 0，完整流程自动重来一遍。
                       也就是说快路径失败会自己降级，不会卡在这个分支里。 */
                    if (s_fast_reconnect && s_idx == STEP_AT)
                    {
                        printf("↻ 重连：跳过 WiFi 配置，直接重建 TCP\r\n");
                        s_idx = STEP_CIPMODE;
                    }
                    s_fast_reconnect = 0;

                    if (++s_idx >= NET_STEP_COUNT)
                    {
                        /* 表走完 = CIPSEND 回 ">" 了，通道正式可写。
                           printf 走 usart.c 里重定向的 _write()，最终从 USART1 输出，
                           和模块所在的 USART2 是分开的，不会污染数据通道。 */
                        Uart_FlushRx(UART_AT);
                        s_state = NET_TRANSPARENT;
                        printf("已进入透传模式，通道交给上层\r\n");
                    }
                    else
                    {
                        s_state = NET_SEND;     /* 还有下一条，回去发 */
                    }
                    break;
                }
                step_failed();              /* 应答里明确说了不对 */
                break;
            }

            /* 没凑出完整应答，才轮到判断超时。
               用「当前时刻 - 发送时刻」而不是自己累加计数：
               Board_Millis 是毫秒系统节拍，差值法天然不怕计数器回绕（约 49 天绕一次）。 */
            if ((Board_Millis() - s_tick) >= k_net_steps[s_idx].timeout_ms)
            {
                step_failed();              /* 超时 = 没等到应答，按失败处理 */
            }
            break;

        /* ---------- 状态三：透传中 ---------- */
        case NET_TRANSPARENT:
        default:
            /* 什么都不做。从这里开始 USART2 上跑的是二进制报文，不能再发 AT
               —— 发了也只会被当成数据打到服务器上。
               上层通过 ESP8266_IsTransparent() 判断是否接管这条通道。 */
            break;
    }
}

/**
  * 函    数：ESP8266_Loop —— 对外唯一接口
  * 参    数：无
  * 返 回 值：无
  * 注    意：包一层是为了以后在配网之外再加别的 AT 流程，到时候在这里按状态
  *           分派即可，main 里的调用代码一个字都不用改。
  */
void ESP8266_Loop(void)
{
    AT_Loop();
}

/**
  * 函    数：ESP8266_IsTransparent —— 查询透传通道是否就绪
  * 参    数：无
  * 返 回 值：1 = 已进入透传，可以往 USART2 上发二进制报文；0 = 还在配网
  * 注    意：只读一个 static 变量，随时可调、无副作用。
  *           给上层 MQTT 状态机做分派用；本文件在透传态下不会再发 AT。
  */
uint8_t ESP8266_IsTransparent(void)
{
    return (s_state == NET_TRANSPARENT) ? 1 : 0;
}

/**
  * 函    数：ESP8266_ExitTransparent —— 把已经死掉的透传通道打回起点
  * 参    数：无
  * 返 回 值：无
  * 注    意：给 MQTT 层判定掉线时调用。做法不是自己去发 "+++"，
  *           而是把状态机退回第 0 步，让配网流程自己处理 —— 第 0 步发 AT
  *           如果没回应，1.5 秒后会自动补一个 "+++" 把模块拽出透传。
  *
  *           这样做的原因：+++ 要求前后各有 1 秒静默，而"什么时候真正安静
  *           下来"只有配网状态机自己清楚。在这里硬发一个 +++，很可能夹在
  *           上一次 PINGREQ 的尾巴上，模块不认，反而更乱。
  *           代价是多等 1.5 秒，比起"要猜时机"，这个代价值得。
  */
void ESP8266_ExitTransparent(void)
{
    if (s_state != NET_TRANSPARENT)
    {
        return;                     /* 不在透传里，没什么可退的 */
    }

    printf("↻ 重走配网流程（AT 探测 → 连 AP → 重建 TCP）\r\n");

    Uart_FlushRx(UART_AT);          /* 冲掉旧连接留下的残渣，别让新会话读到 */
    s_state     = NET_SEND;
    s_idx       = 0;
    s_retry     = 0;
    s_fail_used = 0;
    s_tick      = Board_Millis();
    s_rsp_len   = 0;
    s_rsp[0]    = '\0';

    /* 掉线重连不需要重新配 WiFi：参数在 Flash 里，AP 也大概率还在。
       AT 探测一通过就直奔 CIPSTART，省掉四条指令和一次 Flash 写入。
       快路径真连不上会自动退回完整流程，这里不用额外兜底。 */
    s_fast_reconnect = 1;
}
