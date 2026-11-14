import argparse
import getpass
import hashlib
import json
import sys
import time
import warnings
from pathlib import Path

import serial
from serial.tools.list_ports import comports


PREFIX = "@sysu "
MAX_CREDENTIAL_BYTES = 127
ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "build"
BACKUPS = ROOT / ".flash-backup"
STAGES = {
    "idle": "等待配置",
    "scan": "扫描校园网",
    "scan_error": "扫描失败",
    "no_ap": "板子未扫到 SYSU-SECURE-2.4G",
    "ap_security": "扫到了同名网络，安全模式与 WPA2-Enterprise 不符",
    "needs_setup": "扫到可用的校园网信号",
    "clock": "板上时间需要校准，请运行 retry",
    "auth": "校园网认证",
    "dhcp": "获取 IPv4 地址",
    "https": "HTTPS 出站检查",
    "online": "校园网和 HTTPS 均已通过",
}
WIFI_REASONS = {
    "23": "802.1X 拒绝认证，检查账号、证书和在线设备数",
    "201": "尝试连接时找不到接入点",
    "202": "接入点认证失败",
    "203": "与接入点关联失败",
    "204": "Wi-Fi 握手超时",
    "210": "找不到安全模式兼容的接入点",
}


def choose_port(port):
    if port:
        return port
    candidates = [info.device for info in comports() if info.vid == 0x303A]
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise ValueError("没有找到乐鑫 USB 串口。请接回数据线，或用 --port 指定串口。")
    raise ValueError(f"找到多个乐鑫串口（{', '.join(candidates)}），请用 --port 指定。")


class CampusSerial:
    def __init__(self, port):
        self.connection = serial.Serial(baudrate=115200, timeout=0.3, write_timeout=2)
        self.connection.port = port
        # macOS raises DTR+RTS on open; dropping DTR while RTS is high is the
        # ESP32-S3 USB-Serial-JTAG reset signal. Keep DTR high, drop only RTS.
        self.connection.dtr = True
        self.connection.rts = False
        self.pending = bytearray()

    def __enter__(self):
        self.connection.open()
        return self

    def __exit__(self, *_):
        self.connection.close()

    def write(self, text):
        self.connection.write((text + "\n").encode("ascii"))

    def read_message(self):
        self.pending.extend(self.connection.read_until(size=1024))
        if not self.pending.endswith(b"\n"):
            if len(self.pending) > 4096:
                self.pending.clear()
            return ""
        line = self.pending.decode("utf-8", errors="replace").strip()
        self.pending.clear()
        if line.startswith(PREFIX):
            return line[len(PREFIX):]
        return ""

    def wait_for_firmware(self):
        # A PC-powered boot may still be scanning or connecting before it reads commands.
        deadline = time.monotonic() + 65
        next_hello = 0.0
        while time.monotonic() < deadline:
            if time.monotonic() >= next_hello:
                self.write("\nHELLO")
                next_hello = time.monotonic() + 2
            if self.read_message() == "hello v=1":
                return
        raise ValueError("板子没有回应校园网固件。首次使用请先备份并烧录，再核对当前 COM 号。")

    def request(self, text, *, show=True):
        self.write(text)
        deadline = time.monotonic() + 70
        messages = []
        while time.monotonic() < deadline:
            line = self.read_message()
            if line == "done":
                return messages
            if line and not line.startswith("hello "):
                messages.append(line)
                if show:
                    show_message(line)
        raise ValueError("等待板子回应超时。可运行 status 检查本次结果。")


def parse_message(line):
    label, *items = line.split()
    fields = dict(item.split("=", 1) for item in items if "=" in item)
    return label, fields


def show_message(line):
    label, fields = parse_message(line)
    stage = fields.get("stage")
    description = STAGES.get(stage, stage)
    if label == "progress":
        print(f"正在检查 · {description}", flush=True)
    elif label == "status":
        saved = "已保存" if fields.get("profile") == "1" else "待配置"
        print(f"登录配置 · {saved}", flush=True)
        if fields.get("storage") == "0":
            print("板载配置分区读取失败。", flush=True)
        if fields.get("reset") == "9":
            print("本次启动前发生过低电压复位。", flush=True)
    elif label == "cold_boot" and not fields:
        print("独立供电 · 还没有这份登录配置的冷启动记录。", flush=True)
    elif label == "cold_power":
        if fields.get("reset") == "brownout":
            print("  独立供电记录来自低电压复位后的启动。", flush=True)
        else:
            print("  独立供电记录来自普通上电。", flush=True)
    elif label in ("current", "scan", "cold_boot"):
        title = {"current": "本次启动", "scan": "扫描结果", "cold_boot": "独立供电"}[label]
        print(f"{title} · {description} · 启动编号 {fields['boot']}", flush=True)
        if fields.get("channel", "0") != "0":
            print(f"  信道 {fields['channel']}，信号 {fields['rssi']} dBm")
        if fields.get("ip") not in (None, "0.0.0.0"):
            print(f"  DHCP 已分配 IPv4 {fields['ip']}")
        if int(fields.get("http", "0")) > 0:
            print(f"  HTTP {fields['http']}")
        if fields.get("reason", "0") != "0":
            reason = fields["reason"]
            print(f"  Wi-Fi 原因码 {reason} · {WIFI_REASONS.get(reason, '请查看串口诊断')}")
        if fields.get("err") not in (None, "ESP_OK"):
            print(f"  {fields['err']}")
        if fields.get("tls", "0") != "0" or fields.get("flags", "0x0") != "0x0":
            tls_error = int(fields.get("tls", "0"))
            print(f"  TLS {tls_error:#x}，证书标志 {fields['flags']}")
        sys.stdout.flush()
    elif label == "https_error":
        print(f"  HTTPS 传输错误 {fields['transport']}，套接字错误码 {fields['errno']}", flush=True)
    elif label == "setup" and fields.get("saved") == "1":
        print("NetID 登录配置已写入板载闪存。", flush=True)
    elif label == "setup" and fields.get("saved") == "0":
        print("这次未拿到 IPv4，已保留之前的板载配置。", flush=True)
    elif label == "clear" and fields.get("err") == "ESP_OK":
        print("板载登录配置已清除。", flush=True)
    elif label == "clock" and fields.get("ok") == "1":
        print("已用电脑时间校准板子。", flush=True)
    elif label == "muse" and "text" in fields:
        print(f"[Muse 提醒] {decode_hex(fields['text'])}", flush=True)
    elif label == "agent" and "text" in fields:
        print(f"[AI 推送] {decode_hex(fields['text'])}", flush=True)
    elif label == "dash":
        show_dash(fields)
    elif label in ("token", "bemfa") and "saved" in fields:
        name = "Hub 设备令牌" if label == "token" else "巴法云 UID"
        print(f"{name}{'已写入板载闪存' if fields['saved'] == '1' else '写入失败 ' + fields.get('err', '')}。", flush=True)
    elif label in ("token", "bemfa") and "cleared" in fields:
        print("已从板子清除。" if fields["cleared"] == "1" else "清除失败。", flush=True)
    else:
        print(line, flush=True)


def decode_hex(text):
    try:
        return bytes.fromhex(text).decode("utf-8", errors="replace")
    except ValueError:
        return text


FRESH = {"live": "实时", "stale": "数据过期", "offline": "离线", "none": "暂无数据"}


def show_dash(f):
    yes = lambda k: "有" if f.get(k) == "1" else "无"
    print(f"仪表盘 · Hub 令牌{yes('token')} · 巴法云 UID{yes('bemfa')} · "
          f"MQTT {'已连接' if f.get('mqtt') == '1' else '未连接'} · 演示模式{yes('demo')}", flush=True)
    ago = f.get("last_ok_ago", "-1")
    last = "从未成功" if ago == "-1" else f"{ago} 秒前"
    print(f"  Hub 数据 {FRESH.get(f.get('fresh'), f.get('fresh'))}，上次成功 {last}，"
          f"连续失败 {f.get('failures')} 次；运行记录 {f.get('runs')} 条，服务器 {f.get('servers')} 台", flush=True)
    print(f"  Muse 提醒 {f.get('muse')} 条 · LED GPIO {f.get('led')} · "
          f"内部堆剩余 {int(f.get('heap', 0)) // 1024} KB（最低 {int(f.get('min_heap', 0)) // 1024} KB）· "
          f"PSRAM 剩余 {int(f.get('psram', 0)) // 1024} KB · 已运行 {f.get('uptime', '?')} 秒", flush=True)
    if "mi" in f:
        mi = "hub 未配置" if f["mi"] == "-1" else f"{f['mi']} 台设备，" + ("已连接" if f.get("mi_ok") == "1" else "连接中断")
        print(f"  米家 {mi}", flush=True)


def encode_credential(text, name):
    encoded = text.encode("utf-8")
    if not 1 <= len(encoded) <= MAX_CREDENTIAL_BYTES or any(b < 32 or b == 127 for b in encoded):
        raise ValueError(f"{name}需要 1 到 {MAX_CREDENTIAL_BYTES} 个 UTF-8 字节，且不能含控制字符。")
    return encoded.hex()


def setup(device):
    scans = map(parse_message, device.request("SCAN"))
    if not any(label == "scan" and fields.get("stage") == "needs_setup"
               for label, fields in scans):
        return 1
    netid = encode_credential(input("NetID：").strip(), "NetID")
    with warnings.catch_warnings():
        warnings.simplefilter("error", getpass.GetPassWarning)
        password = encode_credential(getpass.getpass("NetID 密码（隐藏输入）："), "密码")
    replies = device.request(f"SETUP {int(time.time())} {netid} {password}")
    if "setup saved=1" not in replies:
        return 1
    results = map(parse_message, replies)
    return 0 if any(label == "current" and fields.get("stage") == "online"
                    for label, fields in results) else 1


def read_secret(args, prompt):
    if args.value_file:
        return Path(args.value_file).read_text(encoding="utf-8").strip()
    with warnings.catch_warnings():
        warnings.simplefilter("error", getpass.GetPassWarning)
        return getpass.getpass(prompt).strip()


def monitor(port):
    print("串口监视中（Ctrl-C 退出）。Muse 提醒会自动解码成中文。", flush=True)
    conn = serial.Serial(baudrate=115200, timeout=0.5)
    conn.port = port
    conn.dtr = True  # see CampusSerial: avoids the DTR=0/RTS=1 reset pattern on open
    conn.rts = False
    with conn:
        pending = b""
        while True:
            pending += conn.read(1024)
            while b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                if line.startswith(PREFIX):
                    show_message(line[len(PREFIX):])
                elif line:
                    print(line, flush=True)


def flash_size_in_bytes(size):
    if size and size.endswith("MB"):
        return int(size[:-2]) * 1024 * 1024
    raise ValueError("无法确定板载 Flash 容量。")


def backup_original(esp, size, read_flash):
    mac = bytes(esp.read_mac()).hex()
    matching = list(BACKUPS.glob(f"original-{mac}-*.bin"))
    if len(matching) > 1:
        raise ValueError("这块板已有多份原始 Flash 备份，请先核对 .flash-backup。")
    if matching:
        path = matching[0]
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if path.stat().st_size != size or path.stem.rsplit("-", 1)[1] != digest:
            raise ValueError("原始 Flash 备份的容量或 SHA-256 不匹配。")
    else:
        print("正在读取原始 Flash 全盘备份…", flush=True)
        data = read_flash(esp, 0, size)
        if len(data) != size or hashlib.md5(data).hexdigest() != esp.flash_md5sum(0, size):
            raise ValueError("读取的备份与板上 Flash 校验不一致。")
        digest = hashlib.sha256(data).hexdigest()
        BACKUPS.mkdir(exist_ok=True)
        path = BACKUPS / f"original-{mac}-{digest}.bin"
        partial = path.with_suffix(".partial")
        partial.write_bytes(data)
        if hashlib.sha256(partial.read_bytes()).hexdigest() != digest:
            raise ValueError("备份写入电脑后校验不一致。")
        partial.replace(path)
    print(f"原始 Flash 备份已校验 · {path}", flush=True)


def flash(port, already_in_bootloader):
    from esptool import FatalError
    from esptool.cmds import (
        attach_flash, connect_esp, detect_flash_size, erase_region, read_flash,
        reset_chip, run_stub, write_flash,
    )

    metadata_path = BUILD / "flasher_args.json"
    if not metadata_path.is_file():
        raise ValueError("找不到编译产物。请先运行 .\\esp.ps1 build。")
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    if metadata["extra_esptool_args"]["chip"] != "esp32s3":
        raise ValueError("编译产物的目标芯片应为 ESP32-S3。")
    images = {int(offset, 0): (BUILD / name).read_bytes()
              for offset, name in metadata["flash_files"].items()}
    partition_table = images[0x8000]
    # Locate the private partition in the binary table that will actually be written.
    entries = (partition_table[offset:offset + 32] for offset in range(0, len(partition_table), 32))
    campus_partition = next(
        (entry for entry in entries
         if entry[:4] == b"\xaa\x50\x01\x02" and entry[12:28].rstrip(b"\0") == b"campus_nvs"),
        None,
    )
    if campus_partition is None:
        raise ValueError("编译产物里缺少校园网配置分区。")
    campus_offset = int.from_bytes(campus_partition[4:8], "little")
    campus_size = int.from_bytes(campus_partition[8:12], "little")
    try:
        mode = "no-reset" if already_in_bootloader else "default-reset"
        with connect_esp(port=port, before=mode, connect_attempts=2) as rom:
            if rom.CHIP_NAME != "ESP32-S3":
                raise ValueError(f"串口上的芯片是 {rom.CHIP_NAME}，当前固件目标为 ESP32-S3。")
            if rom.secure_download_mode or rom.get_secure_boot_enabled() or rom.get_flash_encryption_enabled():
                raise ValueError("板子启用了安全烧录、Secure Boot 或 Flash 加密，需要匹配的固件。")
            if rom.flash_type():
                raise ValueError("板子使用 Octal Flash，需要匹配的 Flash 模式重新编译。")
            esp = run_stub(rom)
            attach_flash(esp)
            flash_size = detect_flash_size(esp)
            size = flash_size_in_bytes(flash_size)
            print(f"已确认 {rom.get_chip_description()} · Flash {flash_size}", flush=True)
            if size < 4 * 1024 * 1024 or size > 32 * 1024 * 1024:
                raise ValueError("当前编译适用于 4 至 32 MB 的 ESP32-S3 Flash。")
            backup_original(esp, size, read_flash)
            previous_table = read_flash(esp, 0x8000, len(partition_table), no_progress=True)
            if previous_table != partition_table:
                erase_region(esp, campus_offset, campus_size)
            settings = metadata["flash_settings"]
            write_flash(esp, sorted(images.items()), flash_freq=settings["flash_freq"],
                        flash_mode=settings["flash_mode"], flash_size=flash_size, compress=True)
            print("烧录已校验。若板子仍在下载模式，松开 BOOT 后短按 EN / RST。", flush=True)
            reset_chip(esp)
    except FatalError as error:
        raise ValueError(str(error)) from None
    return 0


def main():
    parser = argparse.ArgumentParser(description="ESP32 校园网 USB 串口工具")
    parser.add_argument("action", choices=("flash", "setup", "status", "scan", "retry", "clear",
                                           "dash", "token", "bemfa", "lang", "demo", "muse",
                                           "led", "monitor", "cat", "agent", "mi", "knob"))
    parser.add_argument("value", nargs="?",
                        help="lang: zh/en；demo: ok/gh_failed/gh_running/server_down/off；"
                             "muse: 提醒文字；led: GPIO 号或 off；token/bemfa: clear")
    parser.add_argument("--port", help="指定串口，例如 COM5 或 /dev/cu.usbmodem1101")
    parser.add_argument("--bootloader", action="store_true", help="已手动进入下载模式")
    parser.add_argument("--value-file", help="token/bemfa：从文件读取，而不是隐藏输入")
    args = parser.parse_args()
    try:
        port = choose_port(args.port)
        print(f"正在连接 {port}…", flush=True)
        if args.action == "flash":
            return flash(port, args.bootloader)
        if args.action == "monitor":
            return monitor(port)
        with CampusSerial(port) as device:
            device.wait_for_firmware()
            if args.action == "setup":
                return setup(device)
            if args.action == "clear" and input("清除板载 NetID 登录配置？输入 YES 确认：") != "YES":
                return 1
            if args.action in ("token", "bemfa"):
                if args.value == "clear":
                    command = "clear"
                else:
                    prompt = "Hub 设备令牌（隐藏输入）：" if args.action == "token" else "巴法云私钥 UID（隐藏输入）："
                    command = read_secret(args, prompt).encode("utf-8").hex()
                replies = device.request(f"{args.action.upper()} {command}")
                return 0 if any("saved=1" in r or "cleared=1" in r for r in replies) else 1
            if args.action == "mi":
                if not args.value:
                    raise ValueError("用法：mi lamp on|off|bri 60")
                device.request("MI " + " ".join(args.value.split()))
                return 0
            if args.action == "knob":
                if not args.value:
                    raise ValueError("用法：knob cw|ccw [n] | hold cw|ccw [n] | press | long | rev")
                device.request("KNOB " + " ".join(args.value.split()))
                return 0
            if args.action == "cat":
                device.request("CAT " + " ".join(args.value.split()) if args.value else "CAT auto")
                return 0
            if args.action in ("lang", "demo", "led"):
                if not args.value:
                    raise ValueError(f"{args.action} 需要一个参数。")
                device.request(f"{args.action.upper()} {args.value}")
                return 0
            if args.action == "agent":
                kind, _, text = (args.value or "done").partition(" ")
                payload = json.dumps({"v": 1, "s": "claude", "k": kind, "p": "ESPdev", "h": "test",
                                      "m": text or "这是一条测试推送"}, ensure_ascii=False)
                device.request("AGENT " + payload.encode("utf-8").hex())
                return 0
            if args.action == "muse":
                text = (args.value or "这是一条测试提醒。").encode("utf-8")
                if len(text) > 256:
                    raise ValueError("Muse 提醒最多 256 字节。")
                device.request(f"MUSE {text.hex()}")
                return 0
            if args.action == "retry":
                device.request(f"CLOCK {int(time.time())}")
                device.request("STATUS")
            elif args.action == "clear":
                return 0 if "clear err=ESP_OK" in device.request("CLEAR") else 1
            else:
                device.request(args.action.upper())
            return 0
    except getpass.GetPassWarning:
        print("请在本机 PowerShell 或 Windows Terminal 运行 setup，以便隐藏输入密码。", file=sys.stderr)
    except (ValueError, OSError, EOFError, serial.SerialException) as error:
        print(str(error), file=sys.stderr)
    except KeyboardInterrupt:
        print("\n已退出。", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
