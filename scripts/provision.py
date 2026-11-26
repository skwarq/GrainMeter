#!/usr/bin/env python3
"""Writes config/device.conf into a GrainMeter.

  provision.py device --port /dev/ttyACM0 --config config/device.conf [--test-upload]
      ESP32 over the USB console: every non-empty key is sent as
      "set <key> <value>" and the device's answer is checked. Secret values
      are never printed (the device never returns them either).
  provision.py sim --config config/device.conf --sim-conf pc-data/grainmeter-pc.conf
      merges the same keys into the PC simulator's configuration file.

Empty values are skipped (the device keeps its current setting).
"""
import argparse
import os
import re
import sys
import tempfile
import time
import urllib.parse

# Order used by the per-key fallback (firmware without "apply"): fields
# that others depend on come first (gateway/netmask before static_ip).
KEYS = [
    "svc_pass", "name", "wifi_ssid", "wifi_pass", "report_url", "ts_channel",
    "ts_key", "diag_channel", "diag_key", "upload_s", "measure_s",
    "diag_every_h", "batt_low_mv", "batt_crit_mv", "batt_capacity_mah", "update_url",
    "update_check_h", "gateway", "netmask", "dns", "static_ip",
    "alarm_temp_c", "alarm_rise_c", "alarm_rise_h", "alarm_repeat_min",
    "notify_ntfy_url", "notify_tg_token", "notify_tg_chat",
    "log_udp", "log_dest", "log_level", "log_host", "log_port", "lan_wake", "ap_delay_s",
    "remote_url", "remote_user", "remote_pass", "remote_id",
    "talkback_id", "talkback_key", "remote_budget_min", "remote_idle_min",
]
SECRET = {"svc_pass", "wifi_pass", "ts_key", "diag_key", "notify_tg_token",
          "remote_pass", "talkback_key"}
# Local-only keys (browser login for remote.html, used by scripts/remote_link.py
# and tests); accepted in device.conf but never sent to the device.
LOCAL_ONLY = {"web_url", "web_user", "web_pass"}
REQUIRED = ["svc_pass", "wifi_ssid", "ts_channel", "ts_key"]
PROMPT = "> "


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def load_config(path: str) -> dict:
    if not os.path.exists(path):
        fail(f"{path} not found - run: make device-config")
    mode = os.stat(path).st_mode & 0o077
    if mode:
        print(f"warning: {path} is readable by others (chmod 600 {path})")
    values = {}
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                fail(f"{path}:{n}: expected key=value")
            key, value = line.split("=", 1)
            key, value = key.strip(), value.strip()
            if key in LOCAL_ONLY:
                continue
            if key not in KEYS:
                fail(f"{path}:{n}: unknown key '{key}'")
            if value:
                values[key] = value
    return values


def validate(values: dict, require: bool) -> None:
    missing = [k for k in REQUIRED if k not in values]
    if missing and require:
        fail("fill in: " + ", ".join(missing))
    if "svc_pass" in values and not 8 <= len(values["svc_pass"]) <= 63:
        fail("svc_pass: 8-63 characters")
    if "svc_pass" in values and " " in values["svc_pass"]:
        fail("svc_pass: no spaces")
    for k in ("ts_key", "diag_key"):
        if k in values and not re.fullmatch(r"[A-Za-z0-9]{8,32}", values[k]):
            fail(f"{k}: 8-32 letters/digits (Write API Key)")
    if "talkback_key" in values and not re.fullmatch(r"[A-Za-z0-9]{8,32}", values["talkback_key"]):
        fail("talkback_key: 8-32 letters/digits (TalkBack API Key)")
    for k in ("ts_channel", "diag_channel", "talkback_id"):
        if k in values and not values[k].isdigit():
            fail(f"{k}: number (Channel ID)")
    if "batt_capacity_mah" in values:
        capacity = values["batt_capacity_mah"]
        if not re.fullmatch(r"[0-9]+", capacity) or not 100 <= int(capacity) <= 20000:
            fail("batt_capacity_mah: 100..20000 mAh")
    for k, v in values.items():
        if "\n" in v or "\r" in v or len(v) > 200:
            fail(f"{k}: invalid value")


def shown(key: str, value: str) -> str:
    return "******" if key in SECRET else value


# ---------------------------------------------------------------- device --

class Console:
    def __init__(self, port: str):
        import serial  # pyserial (in the ESP-IDF image)
        self.serial_mod = serial
        self.port = port
        self.ser = None

    def open(self, wait_s: float = 10.0) -> None:
        deadline = time.time() + wait_s
        while True:
            try:
                self.ser = self.serial_mod.Serial(self.port, 115200, timeout=0.2)
                return
            except (OSError, self.serial_mod.SerialException):
                if time.time() > deadline:
                    raise
                time.sleep(0.3)

    def write(self, text: str) -> None:
        self.ser.write(text.encode("utf-8"))
        self.ser.flush()

    def read_until_prompt(self, timeout: float) -> str:
        """Reads until a fresh prompt line ("\n> ") appeared; log lines the
        firmware prints afterwards do not matter."""
        buf = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self.ser.read(256)
            if chunk:
                buf += chunk.decode("utf-8", "replace")
                if "\n" + PROMPT in buf or buf.startswith(PROMPT):
                    time.sleep(0.05)
                    buf += self.ser.read(self.ser.in_waiting or 1).decode("utf-8", "replace")
                    return buf
        return buf

    @staticmethod
    def has_prompt(text: str) -> bool:
        return "\n" + PROMPT in text or text.startswith(PROMPT)

    def reset(self) -> None:
        # USB Serial/JTAG: RTS pulse resets the chip (same as esptool).
        self.ser.dtr = False
        self.ser.rts = True
        time.sleep(0.2)
        self.ser.rts = False
        try:
            self.ser.close()
        except Exception:
            pass
        time.sleep(1.0)
        self.open(wait_s=15.0)

    def wake(self) -> None:
        self.write("\n")
        if self.has_prompt(self.read_until_prompt(2.0)):
            return
        print("== resetting the board to reach the console")
        self.reset()
        deadline = time.time() + 20.0
        while time.time() < deadline:
            self.write("\n")
            if self.has_prompt(self.read_until_prompt(1.0)):
                return
        fail("no console prompt - press RST on the board and run again")

    def command(self, line: str, label: str, timeout: float = 8.0) -> str:
        """label is printed on errors instead of the line (which may hold a
        secret value)."""
        self.ser.reset_input_buffer()
        self.write(line + "\n")
        out = self.read_until_prompt(timeout)
        if not self.has_prompt(out):
            fail(f"no answer from the device to '{label}'")
        return out


def provision_device(args, values: dict) -> None:
    con = Console(args.port)
    con.open()
    con.wake()
    print(f"== writing {len(values)} setting(s) to {args.port}")
    # One "apply" line: all fields validated together and saved atomically
    # (static IP needs gateway + netmask, upload >= measure interval, ...).
    body = "&".join(f"{k}={urllib.parse.quote(values[k], safe='')}" for k in KEYS if k in values)
    out = con.command(f"apply {body}", "apply")
    lines = [l.strip() for l in out.splitlines()]
    # Only the device's own status lines are inspected; the echoed input
    # (which contains the values) is never printed.
    if "configuration saved" in lines:
        for key in KEYS:
            if key in values:
                print(f"   {key:<15} = {shown(key, values[key])}  ok")
    elif any(l.startswith("unknown command 'apply'") for l in lines):
        print("   firmware without 'apply': setting fields one by one")
        for key in KEYS:
            if key not in values:
                continue
            out = con.command(f"set {key} {values[key]}", f"set {key}")
            if f"{key} saved" in out:
                print(f"   {key:<15} = {shown(key, values[key])}  ok")
                continue
            err = next((l.strip() for l in out.splitlines() if l.strip().startswith("error:")), "no confirmation")
            fail(f"{key}: {err}")
    else:
        err = next((l for l in lines if l.startswith("error:")), "no confirmation")
        fail(f"nothing was changed - {err}")
    out = con.command("config", "config")
    text = "\n".join(l for l in out.splitlines() if l.strip().startswith("{"))
    print("== device configuration (no secrets):")
    print(text)
    if args.automap:
        out = con.command("automap", "automap", timeout=15.0)
        print("== automap: " + next((l.strip() for l in out.splitlines() if "assigned" in l), "?"))
    if args.test_upload:
        print("== test: measure + upload")
        con.command("measure store", "measure store", timeout=15.0)
        out = con.command("upload", "upload", timeout=90.0)
        result = next((l.strip() for l in out.splitlines() if l.strip().startswith(("ok:", "failed:"))), "")
        print("   " + (result or "no result"))
        if "no samples to send" in result:
            print("   no sensor is mapped to a point - nothing was sent "
                  "(run with AUTOMAP=1 or use 'map' in the console)")
            sys.exit(2)
        if not result.startswith("ok:"):
            sys.exit(2)


# ------------------------------------------------------------------- sim --

def sim_escape(value: str) -> str:
    # pc/platform/file_store.cpp: '\\' escapes a backslash
    return value.replace("\\", "\\\\")


def provision_sim(args, values: dict) -> None:
    stored = dict(values)
    # NVS keys are limited to 15 characters; PC storage uses the same keys.
    for long_key, store_key in (("batt_capacity_mah", "batt_capacity"),
                                ("remote_budget_min", "remote_budget_m"),
                                ("alarm_repeat_min", "alarm_repeat_m")):
        if long_key in stored:
            stored[store_key] = stored.pop(long_key)
    lines = []
    if os.path.exists(args.sim_conf):
        with open(args.sim_conf, encoding="utf-8") as f:
            lines = f.read().splitlines()
    done = set()
    out = []
    for line in lines:
        key = line.split("=", 1)[0] if "=" in line and not line.startswith("#") else None
        if key in stored:
            out.append(f"{key}={sim_escape(stored[key])}")
            done.add(key)
        else:
            out.append(line)
    for key in KEYS:
        key = {"batt_capacity_mah": "batt_capacity",
               "remote_budget_min": "remote_budget_m",
               "alarm_repeat_min": "alarm_repeat_m"}.get(key, key)
        if key in stored and key not in done:
            out.append(f"{key}={sim_escape(stored[key])}")
    os.makedirs(os.path.dirname(os.path.abspath(args.sim_conf)), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(os.path.abspath(args.sim_conf)))
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    os.chmod(tmp, 0o600)
    os.replace(tmp, args.sim_conf)
    for key in KEYS:
        if key in values:
            print(f"   {key:<15} = {shown(key, values[key])}")
    print(f"== written to {args.sim_conf} (restart the simulator)")
    if values.get("report_url", "https://").startswith("https://") and "ts_channel" in values:
        print("   note: reporting to ThingSpeak needs the TLS build: "
              "cmake --workflow --preset dev-tls")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    d = sub.add_parser("device")
    d.add_argument("--port", required=True)
    d.add_argument("--config", required=True)
    d.add_argument("--test-upload", action="store_true")
    d.add_argument("--automap", action="store_true",
                   help="assign unmapped sensors to free points first")
    s = sub.add_parser("sim")
    s.add_argument("--config", required=True)
    s.add_argument("--sim-conf", required=True)
    args = ap.parse_args()

    values = load_config(args.config)
    validate(values, require=args.mode == "device")
    if args.mode == "device":
        provision_device(args, values)
    else:
        if "report_url" not in values:
            # the simulator default (local mock) would otherwise be kept;
            # real channels need https
            values["report_url"] = "https://api.thingspeak.com" if "ts_key" in values else "mock://thingspeak"
        provision_sim(args, values)


if __name__ == "__main__":
    main()
