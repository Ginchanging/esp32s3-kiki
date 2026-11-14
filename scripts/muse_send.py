#!/usr/bin/env python3
"""Send a test alert to the board through Bemfa cloud, exactly the way Muse does.

  python3 scripts/muse_send.py "测试提醒"
The Bemfa UID (console private key) is read from $BEMFA_UID, --uid-file, or a
hidden prompt. It is never printed.
API: POST https://apis.bemfa.com/va/postJsonMsg  {uid, topic, type: 1 (MQTT), msg}
"""
import argparse
import getpass
import json
import os
import sys
import urllib.request
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("message", nargs="?", default="这是一条来自 Mac 的测试提醒。")
    p.add_argument("--topic", default="musealert")
    p.add_argument("--uid-file")
    args = p.parse_args()

    uid = os.environ.get("BEMFA_UID", "")
    if args.uid_file:
        uid = Path(args.uid_file).read_text(encoding="utf-8").strip()
    if not uid:
        uid = getpass.getpass("巴法云私钥 UID（隐藏输入）：").strip()
    if len(args.message.encode("utf-8")) > 256:
        sys.exit("巴法云单条消息最多 256 字节。")

    body = json.dumps({"uid": uid, "topic": args.topic, "type": 1, "msg": args.message}).encode()
    req = urllib.request.Request("https://apis.bemfa.com/va/postJsonMsg", data=body,
                                 headers={"Content-Type": "application/json; charset=utf-8"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        reply = json.load(resp)
    if reply.get("code") == 0:
        print(f"已推送到主题 {args.topic}：{args.message}")
    elif reply.get("code") == 40004:
        sys.exit("巴法云返回 40004：私钥 UID 或主题名不对（主题要先在控制台创建）。")
    else:
        sys.exit(f"巴法云返回：{reply}")


if __name__ == "__main__":
    main()
