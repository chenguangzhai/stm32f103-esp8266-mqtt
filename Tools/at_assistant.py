#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESP8266 AT 指令串口助手

收发共用同一个串口（一根 USB-TTL），发送和接收之间靠手动改接线切换，
所以界面顶部常驻一行提示，避免发完指令忘了把线接回来。

另外还负责：
  1. 实时回显模组返回的每一行，OK / ERROR / +xxxx 分色，一眼看出卡在哪一步
  2. 手动敲或一键发送 AT 指令，可切换结尾符（AT 规范要求 CRLF）
  3. 一键发 +++ 退出透传（这一条不能带换行，单独处理）

运行：C:\\Python311\\python.exe at_assistant.py
或者双击工程根目录的 AT_Assistant.exe
"""

import datetime
import queue
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    messagebox.showerror(
        "缺少依赖",
        "没找到 pyserial，先装一下：\n\n"
        "C:\\Python311\\python.exe -m pip install pyserial",
    )
    sys.exit(1)


# ---------------------------------------------------------------- 配置

BAUDS = ("9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600")
DEFAULT_BAUD = "115200"
ENCODINGS = ("自动", "UTF-8", "GBK")

# 快捷指令：(显示名, 指令, 是否直接发送)
# 带 SSID 的那条只填入输入框不直接发 —— 参数每次都不一样，替用户猜不如让他自己填
QUICK_CMDS = [
    ("AT", "AT", True),
    ("AT+RST 软复位", "AT+RST", True),
    ("AT+GMR 版本", "AT+GMR", True),
    ("AT+RESTORE 恢复出厂", "AT+RESTORE", True),
    ("AT+CWMODE=1", "AT+CWMODE=1", True),
    ("AT+CWAUTOCONN=1", "AT+CWAUTOCONN=1", True),
    ("AT+CWJAP_DEF 连热点", 'AT+CWJAP_DEF="ssid","password"', False),
    ("AT+CWJAP? 当前热点", "AT+CWJAP?", True),
    ("AT+CIPSTA? 本机 IP", "AT+CIPSTA?", True),
    ("AT+CIPSTATUS 网络状态", "AT+CIPSTATUS", True),
    ("AT+CIPMODE=1 透传模式", "AT+CIPMODE=1", True),
    ("AT+CIPSTART 建 TCP", 'AT+CIPSTART="TCP","111.230.92.227",1883', True),
    ("AT+CIPSEND 进透传", "AT+CIPSEND", True),
    ("+++ 退出透传", "+++", True),
]

ENDINGS = (("无", b""), ("CR (\\r)", b"\r"), ("LF (\\n)", b"\n"), ("CRLF (\\r\\n)", b"\r\n"))

# Text 标签配色：绿=成功 红=失败 蓝=系统 紫=模组主动上报 棕=本机发出 灰=二进制报文
TAGS = {
    "ok": "#1a7f37",
    "err": "#c1121f",
    "sys": "#0b5ed7",
    "atr": "#6f42c1",
    "tx": "#8a6d3b",
    "ts": "#9aa0a6",
    "hex": "#8c6d1f",
}

TIP_TEXT = "⚠ 收发同一个口 —— 发 AT 指令前把线接到模组 RX，看回显时再接回串口 1（USART1）"


class AtAssistant:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.ser = None
        self.reader = None
        self.running = threading.Event()
        self.rxq = queue.Queue()
        self.line_buf = b""         # 收字节，不预先解码：块边界会切断 UTF-8 多字节字符
        self.rx_bytes = 0
        self.tx_bytes = 0
        self.history = []
        self.hist_idx = 0
        self.link_desc = ""

        self._build_ui()
        self._refresh_ports()
        self.root.after(30, self._poll)
        self.root.after(200, self._tick)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ------------------------------------------------------------ 界面

    def _build_ui(self):
        self.root.title("ESP8266 AT 串口助手")
        self.root.geometry("1040x720")
        self.root.minsize(860, 560)

        style = ttk.Style()
        try:
            style.theme_use("vista")
        except tk.TclError:
            pass
        style.configure("Send.TButton", font=("Microsoft YaHei UI", 10, "bold"))

        # ---- 顶部：串口参数 ----
        bar = ttk.Frame(self.root, padding=(10, 8, 10, 2))
        bar.grid(row=0, column=0, sticky="ew")

        ttk.Label(bar, text="串口").pack(side="left")
        self.var_port = tk.StringVar()
        self.cmb_port = ttk.Combobox(bar, textvariable=self.var_port, width=34, state="readonly")
        self.cmb_port.pack(side="left", padx=(4, 4))

        self.btn_refresh = ttk.Button(bar, text="刷新", width=6, command=self._refresh_ports)
        self.btn_refresh.pack(side="left")

        ttk.Label(bar, text="波特率").pack(side="left", padx=(12, 0))
        self.var_baud = tk.StringVar(value=DEFAULT_BAUD)
        self.cmb_baud = ttk.Combobox(bar, textvariable=self.var_baud, width=8, values=BAUDS, state="normal")
        self.cmb_baud.pack(side="left", padx=4)

        self.btn_open = ttk.Button(bar, text="打开串口", width=10, command=self._toggle_port)
        self.btn_open.pack(side="left", padx=(8, 0))

        self.btn_clear = ttk.Button(bar, text="清屏", width=6, command=self._clear)
        self.btn_clear.pack(side="right")
        self.btn_save = ttk.Button(bar, text="保存日志", width=9, command=self._save_log)
        self.btn_save.pack(side="right", padx=(0, 6))

        # ---- 常驻提示：收发同口，要改接线 ----
        tk.Label(
            self.root,
            text=TIP_TEXT,
            bg="#fff3cd",
            fg="#7a5c00",
            anchor="w",
            padx=10,
            pady=4,
            font=("Microsoft YaHei UI", 9),
        ).grid(row=1, column=0, sticky="ew", padx=10, pady=(4, 0))

        # ---- 中部：接收区 ----
        mid = ttk.Frame(self.root, padding=(10, 6, 10, 0))
        mid.grid(row=2, column=0, sticky="nsew")
        mid.rowconfigure(0, weight=1)
        mid.columnconfigure(0, weight=1)

        self.txt = tk.Text(
            mid,
            wrap="char",
            font=("Consolas", 10),
            bg="#fafafa",
            fg="#1f2328",
            insertbackground="#1f2328",
            relief="solid",
            borderwidth=1,
            padx=8,
            pady=6,
            state="disabled",
        )
        self.txt.grid(row=0, column=0, sticky="nsew")
        for name, color in TAGS.items():
            self.txt.tag_configure(name, foreground=color)
        self.txt.tag_configure("tx", font=("Consolas", 10, "bold"))

        sb = ttk.Scrollbar(mid, orient="vertical", command=self.txt.yview)
        sb.grid(row=0, column=1, sticky="ns")
        self.txt.configure(yscrollcommand=sb.set)

        # ---- 显示选项 ----
        opts = ttk.Frame(self.root, padding=(10, 4, 10, 2))
        opts.grid(row=3, column=0, sticky="ew")
        self.var_ts = tk.BooleanVar(value=True)
        self.var_hex_show = tk.BooleanVar(value=False)
        self.var_autoscroll = tk.BooleanVar(value=True)
        ttk.Checkbutton(opts, text="时间戳", variable=self.var_ts).pack(side="left")
        ttk.Checkbutton(opts, text="HEX 显示", variable=self.var_hex_show).pack(side="left", padx=10)
        ttk.Checkbutton(opts, text="自动滚动", variable=self.var_autoscroll).pack(side="left")

        ttk.Label(opts, text="编码").pack(side="left", padx=(18, 0))
        self.var_encoding = tk.StringVar(value=ENCODINGS[0])
        ttk.Combobox(
            opts, textvariable=self.var_encoding, width=7, values=ENCODINGS, state="readonly"
        ).pack(side="left", padx=4)

        # ---- 快捷指令 ----
        box = ttk.LabelFrame(self.root, text=" AT 指令（点一下直接发） ", padding=(8, 6, 8, 8))
        box.grid(row=4, column=0, sticky="ew", padx=10, pady=(4, 2))
        for i, (label, cmd, auto) in enumerate(QUICK_CMDS):
            btn = ttk.Button(box, text=label, command=lambda c=cmd, a=auto: self._quick(c, a))
            btn.grid(row=i // 5, column=i % 5, sticky="ew", padx=3, pady=3)
        for c in range(5):
            box.columnconfigure(c, weight=1, uniform="q")

        # ---- 底部：发送区 ----
        send = ttk.Frame(self.root, padding=(10, 4, 10, 4))
        send.grid(row=5, column=0, sticky="ew")
        send.columnconfigure(1, weight=1)

        ttk.Label(send, text="发送").grid(row=0, column=0, padx=(0, 6))
        self.var_input = tk.StringVar()
        self.ent = ttk.Entry(send, textvariable=self.var_input, font=("Consolas", 10))
        self.ent.grid(row=0, column=1, sticky="ew")
        self.ent.bind("<Return>", lambda e: self._do_send())
        self.ent.bind("<Up>", lambda e: self._hist(-1))
        self.ent.bind("<Down>", lambda e: self._hist(1))

        self.btn_send = ttk.Button(send, text="发送", width=8, style="Send.TButton", command=self._do_send)
        self.btn_send.grid(row=0, column=2, padx=(6, 0))

        row2 = ttk.Frame(send)
        row2.grid(row=1, column=0, columnspan=3, sticky="w", pady=(6, 0))
        ttk.Label(row2, text="结尾符").pack(side="left")
        self.var_ending = tk.StringVar(value="CRLF (\\r\\n)")
        for text, _ in ENDINGS:
            ttk.Radiobutton(row2, text=text, value=text, variable=self.var_ending).pack(
                side="left", padx=(6, 0)
            )
        self.var_hex_send = tk.BooleanVar(value=False)
        ttk.Checkbutton(row2, text="HEX 发送", variable=self.var_hex_send).pack(side="left", padx=(16, 0))

        # ---- 状态栏 ----
        self.var_status = tk.StringVar(value="未连接")
        status = ttk.Frame(self.root, padding=(10, 2, 10, 6))
        status.grid(row=6, column=0, sticky="ew")
        ttk.Label(status, textvariable=self.var_status, foreground="#57606a").pack(side="left")

        self.root.rowconfigure(2, weight=1)
        self.root.columnconfigure(0, weight=1)

    # ------------------------------------------------------------ 串口

    def _refresh_ports(self):
        ports = list(list_ports.comports())
        # 蓝牙虚拟串口一大堆，排后面去，真 USB-TTL 更容易挑到
        ports.sort(key=lambda p: ("Bluetooth" in (p.description or ""), p.device))
        items = [f"{p.device}  {p.description}" for p in ports]
        self.cmb_port["values"] = items
        if not items:
            self.var_port.set("")
            self.var_status.set("没找到任何串口 —— 驱动装了吗？USB-TTL 插好了吗？")
        elif self.var_port.get() not in items:
            self.var_port.set(items[0])
            self.var_status.set(f"找到 {len(items)} 个串口")

    def _toggle_port(self):
        if self.ser and self.ser.is_open:
            self._close_port()
        else:
            self._open_port()

    def _open_port(self):
        raw = self.var_port.get()
        if not raw:
            messagebox.showwarning("提示", "先选一个串口。")
            return
        port = raw.split()[0]
        try:
            baud = int(self.var_baud.get())
        except ValueError:
            messagebox.showwarning("提示", "波特率要是数字，ESP8266 默认 115200。")
            return

        try:
            self.ser = serial.Serial(
                port=port,
                baudrate=baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=0.05,
            )
        except Exception as e:
            messagebox.showerror(
                "打不开",
                f"{port} 打开失败：\n\n{e}\n\n"
                "常见原因：被别的串口助手占着、被烧录工具占着、串口选错了。",
            )
            self.ser = None
            return

        # 缓冲区里堆着的是打开之前的历史数据（多半是上电/复位的噪声），丢掉再开始
        stale = self.ser.in_waiting
        if stale:
            self.ser.reset_input_buffer()

        self.line_buf = b""
        self.running.set()
        self.reader = threading.Thread(target=self._read_loop, args=(self.ser,), daemon=True)
        self.reader.start()

        self.link_desc = f"{port}@{baud}"
        self.btn_open.configure(text="关闭串口")
        self.cmb_port.configure(state="disabled")
        self.cmb_baud.configure(state="disabled")
        tip = f"（丢弃了 {stale} 字节打开前的积压）" if stale else ""
        self._sys(f"已打开 {self.link_desc}，8N1{tip}")

    def _close_port(self):
        self.running.clear()
        if self.reader is not None:
            self.reader.join(timeout=0.4)
            self.reader = None
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass
        self.ser = None
        self.btn_open.configure(text="打开串口")
        self.cmb_port.configure(state="readonly")
        self.cmb_baud.configure(state="normal")
        if self.line_buf:
            self._flush_partial()
        self._sys("串口已关闭")

    def _read_loop(self, ser):
        """后台线程：只负责把字节丢进队列，绝不碰界面。"""
        while self.running.is_set():
            try:
                n = ser.in_waiting
                data = ser.read(n if n else 1)
            except Exception as e:
                if self.running.is_set():
                    self.rxq.put(("err", str(e)))
                break
            if data:
                self.rxq.put(("rx", data))

    # ------------------------------------------------------------ 收

    def _poll(self):
        try:
            while True:
                kind, payload = self.rxq.get_nowait()
                if kind == "rx":
                    self._on_rx(payload)
                elif kind == "err":
                    self._sys(f"串口异常：{payload}")
                    self._close_port()
        except queue.Empty:
            pass
        self.root.after(30, self._poll)

    def _on_rx(self, data: bytes):
        self.rx_bytes += len(data)

        if self.var_hex_show.get():
            self._write(" ".join(f"{b:02X}" for b in data) + " ")
            return

        # 关键：字节先攒着，等凑出完整一行再解码。
        # 直接对每个 read 块 decode 会把 UTF-8 汉字从中间切断，切开的半截全变 �
        self.line_buf += data
        while True:
            i = self.line_buf.find(b"\n")
            if i < 0:
                break
            raw, self.line_buf = self.line_buf[:i], self.line_buf[i + 1:]
            self._write_raw_line(raw.rstrip(b"\r"))

        # 一直没换行说明是透传数据，攒太多先倒出来，否则屏幕一片空白
        if len(self.line_buf) > 512:
            self._flush_partial()

    def _flush_partial(self):
        if self.line_buf:
            self._write_raw_line(self.line_buf)
            self.line_buf = b""

    def _decode_line(self, raw: bytes):
        """把一行字节解成文本。返回 (文本, 是否当二进制处理)"""
        # 行内出现原始控制字符（\t 除外）基本就是二进制报文，转义显示才有意义
        if any((b < 0x20 and b not in (0x09, 0x0D)) for b in raw):
            return "", True

        mode = self.var_encoding.get()
        if mode == "GBK":
            return raw.decode("gb18030", "replace"), False
        if mode == "UTF-8":
            return raw.decode("utf-8", "replace"), False
        # 自动：先严格按 UTF-8，解不了再退 GB18030（兼容 GBK 工程）
        try:
            return raw.decode("utf-8"), False
        except UnicodeDecodeError:
            return raw.decode("gb18030", "replace"), False

    def _write_raw_line(self, raw: bytes):
        text, is_binary = self._decode_line(raw)
        if is_binary:
            hexs = " ".join(f"{b:02X}" for b in raw)
            if self.var_ts.get():
                self._write(self._stamp(), "ts")
            self._write(f"[HEX {len(raw)}B] {hexs}\n", "hex")
            return
        self._write_line(text)

    def _write_line(self, line: str):
        s = line.strip()
        tag = ""
        if s == "OK" or s.startswith("OK "):
            tag = "ok"
        elif "ERROR" in s or s == "FAIL" or s.startswith("busy"):
            tag = "err"
        elif s.lower() in ("ready", "wifi ready", "ai-thinker"):
            tag = "sys"
        elif s.startswith("+") or s.startswith("WIFI "):
            tag = "atr"

        if self.var_ts.get():
            self._write(self._stamp(), "ts")
        self._write(line + "\n", tag)

    def _write(self, text: str, tag: str = ""):
        self.txt.configure(state="normal")
        self.txt.insert("end", text, tag)
        if self.var_autoscroll.get():
            self.txt.see("end")
        self.txt.configure(state="disabled")

    @staticmethod
    def _stamp() -> str:
        return datetime.datetime.now().strftime("[%H:%M:%S.%f")[:-3] + "] "

    def _sys(self, msg: str):
        if self.var_ts.get():
            self._write(self._stamp(), "ts")
        self._write(f"# {msg}\n", "sys")

    # ------------------------------------------------------------ 发

    def _quick(self, cmd: str, auto_send: bool):
        if cmd == "+++":
            self._exit_transparent()
            return
        self.var_input.set(cmd)
        self.ent.icursor("end")
        self.ent.focus_set()
        if auto_send:
            self._do_send()

    def _exit_transparent(self):
        """+++ 是唯一的例外：不能带 CRLF，而且前后各要 1 秒静默。"""
        if not (self.ser and self.ser.is_open):
            messagebox.showwarning("提示", "串口没开，发不出去。")
            return
        self._sys("发 +++ 退出透传：3 秒内什么都别发，等模组回 OK")
        try:
            self.ser.write(b"+++")
        except Exception as e:
            self._sys(f"写串口失败：{e}")
            return
        self.tx_bytes += 3
        if self.var_ts.get():
            self._write(self._stamp(), "ts")
        self._write(">> +++\n", "tx")

    def _do_send(self, cmd: str = None):
        text = cmd if cmd is not None else self.var_input.get()
        if text == "":
            return
        if not (self.ser and self.ser.is_open):
            messagebox.showwarning("提示", "串口没开，先点「打开串口」。")
            return

        if self.var_hex_send.get():
            try:
                payload = bytes.fromhex(text.replace(",", " ").replace("0x", ""))
            except ValueError:
                messagebox.showwarning("HEX 格式不对", "写法示例：41 54 0D 0A")
                return
            shown = " ".join(f"{b:02X}" for b in payload)
        else:
            payload = text.encode("utf-8") + dict(ENDINGS)[self.var_ending.get()]
            shown = text

        try:
            self.ser.write(payload)
        except Exception as e:
            self._sys(f"写串口失败：{e}")
            return

        self.tx_bytes += len(payload)
        if self.var_ts.get():
            self._write(self._stamp(), "ts")
        self._write(f">> {shown}\n", "tx")

        if cmd is None:
            if not self.history or self.history[-1] != text:
                self.history.append(text)
            self.hist_idx = len(self.history)
            self.var_input.set("")

    def _hist(self, step: int):
        if not self.history:
            return "break"
        self.hist_idx = max(0, min(len(self.history), self.hist_idx + step))
        self.var_input.set("" if self.hist_idx >= len(self.history) else self.history[self.hist_idx])
        self.ent.icursor("end")
        return "break"

    # ------------------------------------------------------------ 杂项

    def _clear(self):
        self.txt.configure(state="normal")
        self.txt.delete("1.0", "end")
        self.txt.configure(state="disabled")
        self.rx_bytes = 0
        self.tx_bytes = 0

    def _save_log(self):
        path = filedialog.asksaveasfilename(
            title="保存串口日志",
            defaultextension=".log",
            filetypes=[("日志文件", "*.log"), ("文本文件", "*.txt"), ("所有文件", "*.*")],
            initialfile=datetime.datetime.now().strftime("at_%Y%m%d_%H%M%S.log"),
        )
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(self.txt.get("1.0", "end-1c"))
        except Exception as e:
            messagebox.showerror("保存失败", str(e))
            return
        self._sys(f"日志已保存：{path}")

    def _tick(self):
        if self.ser and self.ser.is_open:
            head = f"已连接 {self.link_desc}"
        else:
            head = "未连接"
        self.var_status.set(f"{head}    RX {self.rx_bytes} B    TX {self.tx_bytes} B")
        self.root.after(200, self._tick)

    def _on_close(self):
        self.running.clear()
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass
        self.root.destroy()


def main():
    # 不设 DPI 感知的话，Windows 上界面是被拉伸糊掉的
    try:
        from ctypes import windll

        windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass

    root = tk.Tk()
    app = AtAssistant(root)

    # --selftest：只建界面不跑循环，用来验证代码能起来
    if "--selftest" in sys.argv:
        root.update()
        root.destroy()
        print("selftest OK")
        return

    root.mainloop()


if __name__ == "__main__":
    main()
