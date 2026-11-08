#ifndef __ESP8266_H
#define __ESP8266_H

#include <stdio.h>
#include <stdint.h>      /* uint8_t：以前是靠 uart_app.h 间接带进来的，别再依赖传递 */
#include "uart_app.h"    /* 有意留在 .h：ESP8266.c / mqtt_client.c / main.c 三处都要 Uart_*，删了得各补一遍。别再删 */

/* ==================== 赛场参数（进场只改这一段）====================*/
#define WIFI_SSID           "your-ssid"//你的WiFi名称
#define WIFI_PASS           "your-password"//你的WiFi密码

#define MQTT_HOST           "111.230.92.227"    /* 赛场 MQTT 服务器 */

/* ==================== AT 指令 ====================
   全部集中在这里，改现场参数不用翻 .c 文件。 */

#define AT_             "AT\r\n"                //测试当前是否为AT模式
#define AT_RST          "AT+RST\r\n"            //软复位
#define AT_RESTORE      "AT+RESTORE\r\n"        //恢复出厂设置
#define AT_TRANS_OFF    "AT+SAVETRANSLINK=0\r\n"//清掉 Flash 里残留的透传配置
#define AT_CIPSTA       "AT+CIPSTATUS\r\n"      //查网络状态（STATUS:5 = 未连上 AP）

/* ---------- 连路由器 ---------- */
#define AT_NET1 "AT+CWMODE=1\r\n"   //单 STA 模式
#define AT_NET2 "AT+CWAUTOCONN=1\r\n"//上电自动连接 AP

/* 必须用 _DEF 版本（写 Flash），不能用 AT+CWJAP。
   区别是致命的：AT+CWJAP 只对「本次运行」生效，模块一掉电就忘了；
   而本工程以前的做法是「写 SAVETRANSLINK → 复位 → 靠模块自己重连」，
   复位后模块去找的是 Flash 里那条旧热点（可能早就没了），于是永远卡在
   「有 ready 没有 WIFI GOT IP」。现象：AT+CWJAP 明明回 OK，复位后却再也连不上。
   现在流程不再复位，用 _DEF 是为了掉电重启后也能直接连上。 */
#define AT_NET3 "AT+CWJAP_DEF=\"" WIFI_SSID "\",\"" WIFI_PASS "\"\r\n"//连热点（写入Flash）

/* ---------- 进透传：CIPMODE → CIPSTART → CIPSEND ----------
   和 SAVETRANSLINK 的区别：这条路当场生效，不需要复位，
   而且每一步都有明确应答，连不上会直接回 ERROR / CLOSED。 */
#define AT_CIPMODE  "AT+CIPMODE=1\r\n"  //开透传模式（只是设标志，不建连接）
#define AT_CIPSTART "AT+CIPSTART=\"TCP\",\"%s\",1883\r\n"//建 TCP 连接，%s = 服务器域名
#define AT_CIPSEND  "AT+CIPSEND\r\n"    //收到 ">" 后进入透传
#define EXIT_PASS   "+++"               //退出透传

/* MQTT 服务器地址：定义在上面「赛场参数」段里（MQTT_HOST）。
   这里只说明一件事：Path /mqtt 那种 ws:// + 8083 是给浏览器用的，设备用不了 ——
   裸 TCP 不会做 HTTP Upgrade 握手，broker 会一直等 HTTP 头，永远不回 MQTT 报文。
   所以设备必须走 1883（mqtt://），也就是 AT_CIPSTART 里写的那个端口。 */

void    ESP8266_Loop(void);
uint8_t ESP8266_IsTransparent(void);
void    ESP8266_ExitTransparent(void);

#endif
