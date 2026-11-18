#!/usr/bin/env python3
"""Live mirror of the real board: shows the frames the ESP32-S3 actually
renders (streamed over USB serial) and sends mouse clicks back as touches.

  ./esp.sh mirror            (uses ESP-IDF's Python, which has pyserial + Tk)

Frame lines from the board:  @F x1 y1 x2 y2 <fnv1a hex> <base64 RLE RGB565-LE>
LED colour:                  @L r g b
The command box sends board commands while the window holds the serial port:
  lang en | lang zh | demo gh_failed | demo off | muse 文字 | dash | knob long
Rotary knob stand-in: mouse wheel = page-context turn, Shift + wheel = press and
turn (page navigation), right click = context click.
"""
import argparse
import base64
import queue
import sys
import threading
import time
import tkinter as tk

import serial
from serial.tools.list_ports import comports

W, H, SCALE = 240, 320, 2

# RGB565 (little endian in the stream) -> RGB888 bytes
LUT = [bytes((((v >> 11) & 0x1F) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31))
       for v in range(65536)]


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def rle_decode(data, count):
    out, i, n = [], 0, len(data)
    while i < n and len(out) < count:
        c = data[i]
        i += 1
        if c & 0x80:
            v = data[i] | data[i + 1] << 8
            i += 2
            out.extend([v] * ((c & 0x7F) + 1))
        else:
            k = c + 1
            out.extend(data[i + 2 * j] | data[i + 2 * j + 1] << 8 for j in range(k))
            i += 2 * k
    return out


class Mirror:
    def __init__(self, port):
        self.conn = serial.Serial(baudrate=115200, timeout=0.2)
        self.conn.port = port
        self.conn.dtr = True   # keep DTR high: DTR low + RTS high resets the ESP32-S3
        self.conn.rts = False
        self.conn.open()
        self.fb = bytearray(W * H * 3)
        self.lock = threading.Lock()
        self.dirty = True
        self.frames = self.bytes = self.bad = 0
        self.last_full_req = 0.0
        self.led = (0, 0, 0)
        self.logs = queue.Queue()
        self.running = True
        self.write_lock = threading.Lock()

    def send(self, line):
        with self.write_lock:
            self.conn.write((line + "\n").encode("utf-8"))

    def reader(self):
        pending = b""
        while self.running:
            try:
                chunk = self.conn.read(4096)
            except serial.SerialException as e:
                self.logs.put(f"串口错误：{e}")
                break
            if not chunk:
                continue
            self.bytes += len(chunk)
            pending += chunk
            while b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                self.handle(raw.rstrip(b"\r"))
            if len(pending) > 65536:  # garbage without newlines
                pending = b""

    def handle(self, raw):
        if raw.startswith(b"@F "):
            self.frame(raw)
        elif raw.startswith(b"@L "):
            try:
                self.led = tuple(int(x) for x in raw.split()[1:4])
            except ValueError:
                pass
        else:
            text = raw.decode("utf-8", errors="replace")
            if text.startswith("@sysu muse text="):
                hexs = text.split("=", 1)[1]
                try:
                    text = "[Muse 提醒] " + bytes.fromhex(hexs).decode("utf-8", errors="replace")
                except ValueError:
                    pass
            if text and text not in ("@sysu done",):
                self.logs.put(text)

    def frame(self, raw):
        try:
            _, x1, y1, x2, y2, h, b64 = raw.split(b" ", 6)
            x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)
            data = base64.b64decode(b64, validate=True)
            if int(h, 16) != fnv1a(data) or not (0 <= x1 <= x2 < W and 0 <= y1 <= y2 < H):
                raise ValueError("checksum")
            w = x2 - x1 + 1
            px = rle_decode(data, w * (y2 - y1 + 1))
            if len(px) != w * (y2 - y1 + 1):
                raise ValueError("length")
        except (ValueError, IndexError, base64.binascii.Error):
            self.bad += 1
            now = time.time()
            if now - self.last_full_req > 2:  # a log line got mixed in: redraw everything
                self.last_full_req = now
                self.send("MIRROR full")
            return
        with self.lock:
            k = 0
            for y in range(y1, y2 + 1):
                o = (y * W + x1) * 3
                row = b"".join(LUT[v] for v in px[k:k + w])
                self.fb[o:o + 3 * w] = row
                k += w
            self.dirty = True
            self.frames += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--shot", help="no window: capture the board's real frame to this .ppm and exit")
    ap.add_argument("--wait", type=float, default=6, help="seconds to collect frames for --shot")
    ap.add_argument("--cmd", action="append", default=[], help="board command to send first (repeatable)")
    args = ap.parse_args()
    port = args.port
    if not port:
        ports = [p.device for p in comports() if p.vid == 0x303A]
        if len(ports) != 1:
            sys.exit(f"找到 {len(ports)} 个乐鑫串口，请用 --port 指定。")
        port = ports[0]
    try:
        m = Mirror(port)
    except serial.SerialException as e:
        sys.exit(f"打不开 {port}：{e}\n（串口同一时间只能被一个程序占用，先关掉 monitor 等。）")

    if args.shot:
        threading.Thread(target=m.reader, daemon=True).start()
        m.send("")
        for c in args.cmd:
            word, _, rest = c.partition(" ")
            m.send("MUSE " + rest.encode().hex() if word.lower() == "muse" else word.upper() + (" " + rest if rest else ""))
            time.sleep(0.3)
        m.send("MIRROR on")
        time.sleep(args.wait)
        m.running = False
        m.send("MIRROR off")
        with m.lock:
            data = b"P6\n%d %d\n255\n" % (W, H) + bytes(m.fb)
        with open(args.shot, "wb") as f:
            f.write(data)
        print(f"真板截图 {args.shot}：帧片段 {m.frames}，坏帧 {m.bad}，{m.bytes // 1024} KB", flush=True)
        while not m.logs.empty():
            print(m.logs.get_nowait())
        m.conn.close()
        return

    root = tk.Tk()
    root.title("ESP32-S3 实时镜像")
    root.configure(bg="#0D1117")
    canvas = tk.Canvas(root, width=W * SCALE, height=H * SCALE, highlightthickness=0, bg="black")
    canvas.grid(row=0, column=0, rowspan=3, padx=8, pady=8)
    image_item = canvas.create_image(0, 0, anchor="nw")
    side = tk.Frame(root, bg="#0D1117")
    side.grid(row=0, column=1, sticky="n", padx=(0, 8), pady=8)
    tk.Label(side, text="LED", fg="#8B949E", bg="#0D1117").pack()
    led = tk.Canvas(side, width=60, height=60, highlightthickness=0, bg="#0D1117")
    led.pack()
    led_dot = led.create_oval(8, 8, 52, 52, fill="#111111", outline="#30363D")
    stats = tk.Label(side, text="", fg="#8B949E", bg="#0D1117", justify="left", font=("Menlo", 10))
    stats.pack(pady=(8, 0))
    tk.Label(side, text="命令：\nlang en / lang zh\ndemo gh_failed\ndemo off\nmuse 文字\ndash\nknob long",
             fg="#8B949E", bg="#0D1117", justify="left").pack(pady=(12, 4), anchor="w")
    tk.Label(side, text="旋钮：\n滚轮 = 拧\nShift+滚轮 = 按住拧\n右键 = 按下",
             fg="#8B949E", bg="#0D1117", justify="left").pack(pady=(4, 4), anchor="w")
    entry = tk.Entry(side, width=18)
    entry.pack()

    def run_command(event=None):
        text = entry.get().strip()
        entry.delete(0, "end")
        if not text:
            return
        word, _, rest = text.partition(" ")
        if word.lower() == "muse":
            m.send("MUSE " + (rest or "这是一条测试提醒。").encode("utf-8").hex())
        else:
            m.send(word.upper() + (" " + rest if rest else ""))

    entry.bind("<Return>", run_command)

    state = {"photo": None, "last_motion": 0.0, "down": False}

    def to_board(event):
        return max(0, min(W - 1, event.x // SCALE)), max(0, min(H - 1, event.y // SCALE))

    def press(event):
        state["down"] = True
        x, y = to_board(event)
        m.send(f"TOUCH {x} {y} 1")

    def motion(event):
        if state["down"] and time.time() - state["last_motion"] > 0.03:
            state["last_motion"] = time.time()
            x, y = to_board(event)
            m.send(f"TOUCH {x} {y} 1")

    def release(event):
        state["down"] = False
        x, y = to_board(event)
        m.send(f"TOUCH {x} {y} 0")

    canvas.bind("<ButtonPress-1>", press)
    canvas.bind("<B1-Motion>", motion)
    canvas.bind("<ButtonRelease-1>", release)

    # rotary knob stand-in; trackpads fire many small wheel events, so send at most one
    # step every 120 ms
    def wheel(event):
        if time.time() - state.get("last_wheel", 0.0) < 0.12 or not event.delta:
            return
        state["last_wheel"] = time.time()
        held = event.state & 0x1  # Shift
        m.send(("KNOB hold " if held else "KNOB ") + ("cw" if event.delta < 0 else "ccw"))

    def knob_press(event):
        m.send("KNOB press")

    canvas.bind("<MouseWheel>", wheel)
    canvas.bind("<Button-4>", lambda e: (setattr(e, "delta", 1), wheel(e)))   # X11 wheel up
    canvas.bind("<Button-5>", lambda e: (setattr(e, "delta", -1), wheel(e)))  # X11 wheel down
    canvas.bind("<Button-2>" if sys.platform == "darwin" else "<Button-3>", knob_press)

    t0 = time.time()

    def refresh():
        with m.lock:
            ppm = (b"P6\n%d %d\n255\n" % (W, H) + bytes(m.fb)) if m.dirty else None
            m.dirty = False
        if ppm is not None:
            photo = tk.PhotoImage(data=ppm, format="ppm").zoom(SCALE, SCALE)
            canvas.itemconfigure(image_item, image=photo)
            state["photo"] = photo
        r, g, b = (min(255, c * 4) for c in m.led)
        led.itemconfigure(led_dot, fill=f"#{r:02x}{g:02x}{b:02x}" if (r or g or b) else "#111111")
        el = max(time.time() - t0, 1)
        stats.configure(text=f"{port.split('/')[-1]}\n帧片段 {m.frames}\n坏帧 {m.bad}\n"
                             f"{m.bytes / 1024 / el:.1f} KB/s")
        while not m.logs.empty():
            print(m.logs.get_nowait(), flush=True)
        root.after(40, refresh)

    def close():
        m.running = False
        try:
            m.send("MIRROR off")
            time.sleep(0.1)
            m.conn.close()
        except serial.SerialException:
            pass
        root.destroy()

    threading.Thread(target=m.reader, daemon=True).start()
    m.send("")
    m.send("MIRROR on")
    root.protocol("WM_DELETE_WINDOW", close)
    root.after(40, refresh)
    print(f"镜像 {port}：窗口里看到的是板子真实渲染的画面，点击/拖动 = 触摸。", flush=True)
    root.mainloop()


if __name__ == "__main__":
    main()
