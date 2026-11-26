#!/usr/bin/env python3
"""Read unsigned, unencrypted GrainMeter logs over WSS without waking the device."""
import argparse
from pathlib import Path
import re
import ssl
import sys
from urllib.parse import urlsplit


def read_conf(path):
    values = {}
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line and not line.startswith("#") and "=" in line:
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    required = ("web_url", "web_user", "web_pass", "remote_id")
    missing = [key for key in required if not values.get(key)]
    if missing:
        raise ValueError("Missing configuration keys: " + ", ".join(missing))
    if not re.fullmatch(r"[a-z0-9-]{1,24}", values["remote_id"]):
        raise ValueError("Invalid remote_id (expected 1..24 lowercase letters, digits or hyphens)")
    return values


def broker_url(value):
    try:
        url = urlsplit(value)
        port = url.port or 443
        if (url.scheme != "wss" or not url.hostname or url.username is not None
                or url.password is not None or url.fragment):
            raise ValueError()
        path = url.path or "/mqtt"
        if url.query:
            path += "?" + url.query
        return url.hostname, port, path
    except ValueError:
        raise ValueError("Invalid web_url (expected wss://host[:port]/mqtt)") from None


def plain_text(payload):
    text = payload.decode("utf-8", errors="replace")
    text = re.sub(r"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)", "", text)
    text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    return re.sub(r"[\x00-\x08\x0b-\x1f\x7f]", "", text)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default=Path(__file__).resolve().parents[1] / "config/device.conf")
    parser.add_argument("--status", action="store_true", help="also print UNVERIFIED device status to stderr")
    args = parser.parse_args(argv)
    try:
        conf = read_conf(args.config)
        host, port, path = broker_url(conf["web_url"])
    except (OSError, UnicodeError):
        print("Cannot read configuration file.", file=sys.stderr)
        return 2
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2
    try:
        import paho.mqtt.client as mqtt
        version = mqtt.CallbackAPIVersion.VERSION2
    except (ImportError, AttributeError):
        print("Missing dependency: install with python3 -m pip install 'paho-mqtt>=2,<3'.", file=sys.stderr)
        return 2

    topic = "gm/" + conf["remote_id"] + "/log"
    status_topic = "gm/" + conf["remote_id"] + "/status"
    pending = {}
    result = 0
    client = mqtt.Client(version, protocol=mqtt.MQTTv311, transport="websockets")
    client.username_pw_set(conf["web_user"], conf["web_pass"])
    client.ws_set_options(path=path)
    client.tls_set(cert_reqs=ssl.CERT_REQUIRED)

    def fail(message):
        nonlocal result
        result = 1
        print(message, file=sys.stderr, flush=True)
        client.disconnect()

    def on_connect(client, _userdata, _flags, reason_code, _properties):
        if reason_code.is_failure:
            fail("Broker connection refused. Check broker credentials and permissions.")
            return
        pending.clear()
        for target, qos in [(topic, 0)] + ([(status_topic, 1)] if args.status else []):
            code, mid = client.subscribe(target, qos=qos)
            if code != mqtt.MQTT_ERR_SUCCESS:
                fail("Cannot send subscription request.")
                return
            pending[mid] = target

    def on_subscribe(_client, _userdata, mid, reason_codes, _properties):
        target = pending.pop(mid, None)
        if not reason_codes or any(code.is_failure for code in reason_codes):
            if target == status_topic:
                print("Status access denied; log subscription remains active.", file=sys.stderr, flush=True)
            else:
                fail("Log access denied. Allow Subscribe on " + topic + " for the web broker login.")

    def on_message(_client, _userdata, message):
        if message.topic == topic:
            if message.retain:
                return
            text = plain_text(message.payload)
            sys.stdout.write(text + ("" if text.endswith("\n") else "\n"))
            sys.stdout.flush()
        elif args.status and message.topic == status_topic:
            print("[status UNVERIFIED] " + plain_text(message.payload).rstrip("\n"), file=sys.stderr, flush=True)

    client.on_connect = on_connect
    client.on_subscribe = on_subscribe
    client.on_message = on_message
    try:
        client.connect(host, port, keepalive=30)
        client.loop_forever()
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError, mqtt.WebsocketConnectionError):
        # Never echo exceptions: a broker/URL error may contain credentials.
        print("MQTT connection failed. Check WSS URL, TLS and network access.", file=sys.stderr)
        result = 1
    finally:
        client.disconnect()
    return result


if __name__ == "__main__":
    sys.exit(main())
