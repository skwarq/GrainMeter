#!/usr/bin/env python3
"""Print a remote.html link with the connection form filled from device.conf.

The values travel in the URL fragment (#cfg=...), which the browser never
sends to the server; the page clears it from the address bar after reading.
The link contains passwords: do not paste it into chats or tickets.

usage: scripts/remote_link.py [--config config/device.conf]
                              [--base https://skwarq.github.io/GrainMeter/remote.html]

For a local build: --base http://127.0.0.1:8765/remote.html
"""
import argparse
import base64
import json
import os
import sys


def read_conf(path):
    values = {}
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                if v.strip():
                    values[k.strip()] = v.strip()
    return values


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", default=os.path.join(root, "config", "device.conf"))
    ap.add_argument("--base", default="https://skwarq.github.io/GrainMeter/remote.html")
    args = ap.parse_args()
    c = read_conf(args.config)
    form = {
        "url": c.get("web_url", ""),
        "username": c.get("web_user", ""),
        "password": c.get("web_pass", ""),
        "id": c.get("remote_id", ""),
        "servicePassword": c.get("svc_pass", ""),
        "talkbackId": c.get("talkback_id", ""),
        "talkbackKey": c.get("talkback_key", ""),
        # Dashboard (same origin): channel IDs fill its Settings if empty.
        # Read API keys are not in device.conf (the device has write keys).
        "channelId": c.get("ts_channel", ""),
        "diagChannelId": c.get("diag_channel", ""),
    }
    missing = [k for k in ("web_url", "web_user", "web_pass", "remote_id", "svc_pass") if k not in c]
    if missing:
        print("warning: empty in device.conf: " + ", ".join(missing), file=sys.stderr)
    blob = base64.urlsafe_b64encode(json.dumps({k: v for k, v in form.items() if v}).encode()).decode()
    print(args.base + "#cfg=" + blob)


if __name__ == "__main__":
    main()
