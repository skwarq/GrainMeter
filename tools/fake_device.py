#!/usr/bin/env python3
"""Local GrainMeter LAN console protocol simulator."""
import argparse
import json
import socket
import time
from urllib.parse import quote


def serve_session(conn, args):
    def send(text):
        conn.sendall(text.encode('utf-8'))

    conn.settimeout(0.1)
    send('GM-CONSOLE v1 ssid={} fw=fake-1\r\nAUTH?\r\n'.format(quote(args.ssid, safe='')))
    buffer = b''
    authenticated = False
    failures = 0
    logs = True
    last_line = time.monotonic()
    next_log = last_line + args.log_every
    while True:
        now = time.monotonic()
        if now - last_line >= args.idle_s:
            send('BYE idle\r\n')
            return 'idle'
        if authenticated and logs and now >= next_log:
            send('I (1234) app: simulator alive\r\n')
            next_log = now + args.log_every
        try:
            data = conn.recv(4096)
        except socket.timeout:
            continue
        if not data:
            return 'disconnect'
        buffer += data
        while b'\n' in buffer:
            raw, buffer = buffer.split(b'\n', 1)
            line = raw.rstrip(b'\r').decode('utf-8', 'replace')
            last_line = time.monotonic()
            if not authenticated:
                if line == 'auth ' + args.password:
                    authenticated = True
                    send('AUTH OK\r\n')
                else:
                    failures += 1
                    send('AUTH FAIL\r\n')
                    if failures >= 3:
                        send('BYE auth\r\n')
                        return 'auth'
                continue
            if line == '#ping':
                send('#pong\r\n')
                continue
            if line in ('exit', 'restart'):
                send('BYE ' + line + '\r\n')
                return line
            if line == 'help':
                reply = 'help status echo restart exit logs on/off'
            elif line == 'status':
                reply = json.dumps({'ssid': args.ssid, 'status': 'ok'})
            elif line.startswith('echo '):
                reply = line[5:]
            elif line in ('logs on', 'logs off'):
                logs = line == 'logs on'
                reply = 'Logs ' + ('on' if logs else 'off')
            else:
                reply = 'Unknown command'
            send(reply + '\r\n> ')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--password', default='test')
    parser.add_argument('--ssid', default='GrainMeter-0079')
    parser.add_argument('--name', default='Fake GrainMeter')
    parser.add_argument('--hello-port', type=int, default=5515)
    parser.add_argument('--tcp-port', type=int, default=5516)
    parser.add_argument('--window-ms', type=int, default=5000)
    parser.add_argument('--hello-every', type=float, default=3)
    parser.add_argument('--idle-s', type=float, default=60)
    parser.add_argument('--once', action='store_true')
    parser.add_argument('--hello-addr', default='255.255.255.255')
    parser.add_argument('--bind-ip', default='0.0.0.0')
    parser.add_argument('--ip', default='127.0.0.1', help='IPv4 address advertised in hello')
    parser.add_argument('--log-every', type=float, default=5)
    args = parser.parse_args()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        while True:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
                server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                server.bind((args.bind_ip, args.tcp_port))
                server.listen(1)
                server.settimeout(0.05)
                next_hello = 0
                window_end = 0
                announced = False
                while True:
                    now = time.monotonic()
                    if now >= next_hello and (not args.once or not announced):
                        hello = 'GM-HELLO v1 ssid={} name={} ip={} port={} fw=fake-1 window_ms={}\n'.format(
                            quote(args.ssid, safe=''), quote(args.name, safe=''), args.ip,
                            args.tcp_port, args.window_ms)
                        udp.sendto(hello.encode('ascii'), (args.hello_addr, args.hello_port))
                        announced = True
                        next_hello = now + args.hello_every
                        window_end = now + args.window_ms / 1000
                    if args.once and now > window_end:
                        return
                    try:
                        conn, _ = server.accept()
                    except socket.timeout:
                        continue
                    with conn:
                        if time.monotonic() > window_end:
                            continue
                        try:
                            reason = serve_session(conn, args)
                        except OSError:
                            reason = 'disconnect'
                    break
            if reason == 'exit' or args.once:
                return
            if reason == 'restart':
                time.sleep(2)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
