#!/usr/bin/env python3
"""GrainMeter LAN console, using only the Python standard library."""
import argparse
import codecs
import getpass
import ipaddress
import os
from pathlib import Path
import queue
import socket
import sys
import threading
import time
from urllib.parse import unquote


def status(message):
    print(time.strftime('[%H:%M:%S] ') + message, file=sys.stderr, flush=True)


def parse_hello(data):
    try:
        line = data.decode('ascii')
        if not line.endswith('\n') or len(line.splitlines()) != 1:
            return None
        parts = line.split()
        if parts[:2] != ['GM-HELLO', 'v1']:
            return None
        fields = dict(part.split('=', 1) for part in parts[2:])
        fields = {k: unquote(v, errors='strict') for k, v in fields.items()}
        for key in ('ssid', 'name', 'ip', 'fw', 'window_ms'):
            if not fields.get(key):
                return None
        ipaddress.IPv4Address(fields['ip'])
        fields['port'] = int(fields.get('port', 5516))
        fields['window_ms'] = int(fields['window_ms'])
        if not 0 < fields['port'] < 65536 or fields['window_ms'] < 0:
            return None
        return fields
    except (ValueError, UnicodeError):
        return None


def udp_socket(args):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, 'SO_REUSEPORT'):
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except OSError:
            pass
    sock.bind((args.bind_ip or '', args.hello_port))
    sock.settimeout(0.2)
    return sock


def password_for(args):
    value = args.password
    if value is None:
        value = os.environ.get('GM_SVC_PASS')
    if value is None:
        try:
            for line in Path(args.config).read_text(encoding='utf-8').splitlines():
                key, sep, val = line.strip().partition('=')
                if sep and key.strip() == 'svc_pass':
                    value = val.strip() or None
                    break
        except FileNotFoundError:
            pass
    if value is None:
        value = getpass.getpass('Service password: ')
    if '\n' in value or '\r' in value:
        raise ValueError('Password must be a single line')
    return value


def stdin_reader(lines):
    for line in sys.stdin:
        lines.put(line.rstrip('\r\n'))
    lines.put(None)


class Output:
    """Frame control lines while forwarding ordinary bytes and partial prompts."""
    def __init__(self):
        self.pending = b''
        self.streaming = False
        self.decoder = codecs.getincrementaldecoder('utf-8')('replace')

    def write(self, data):
        sys.stdout.write(self.decoder.decode(data))
        sys.stdout.flush()

    def feed(self, data):
        self.pending += data
        events = []
        while self.pending:
            if self.pending.startswith(b'> ') and not self.streaming:
                self.write(b'> ')
                self.pending = self.pending[2:]
                events.append('prompt')
                continue
            pos = self.pending.find(b'\n')
            if pos >= 0:
                line, self.pending = self.pending[:pos + 1], self.pending[pos + 1:]
                plain = line.rstrip(b'\r\n')
                if not self.streaming and plain == b'#pong':
                    pass
                elif not self.streaming and plain.startswith(b'BYE '):
                    reason = plain[4:].decode('utf-8', 'replace')
                    status('BYE ' + reason)
                    events.append('bye:' + reason)
                else:
                    self.write(line)
                self.streaming = False
                continue
            if not self.streaming and any(prefix.startswith(self.pending) or self.pending.startswith(prefix)
                                          for prefix in (b'#pong\r\n', b'BYE ', b'> ')):
                break
            self.write(self.pending)
            self.pending = b''
            self.streaming = True
        return events


def session(sock, password, args, lines):
    def send(line):
        sock.sendall((line + '\n').encode('utf-8'))

    sock.settimeout(0.2)
    pending = b''
    auth_deadline = time.monotonic() + 10
    authenticated = False
    while not authenticated:
        if time.monotonic() >= auth_deadline:
            raise OSError('Authentication timed out')
        try:
            data = sock.recv(4096)
        except socket.timeout:
            continue
        if not data:
            raise OSError('Disconnected during authentication')
        pending += data
        while b'\n' in pending:
            raw, pending = pending.split(b'\n', 1)
            line = raw.rstrip(b'\r')
            if line == b'AUTH?':
                send('auth ' + password)
            elif line == b'AUTH FAIL':
                status('Authentication failed')
                return 2, 'auth'
            elif line == b'AUTH OK':
                authenticated = True
                break
            elif line.startswith(b'BYE '):
                status(line.decode('utf-8', 'replace'))
                return 2, 'auth'
    output = Output()
    commands = list(args.cmd or [])
    waiting = False
    deadline = 0
    next_ping = time.monotonic() + 10
    exiting = False
    exit_deadline = 0
    if args.no_logs:
        commands.insert(0, 'logs off')
    # The initial prompt may be absent; do not wait for it to start commands.
    events = output.feed(pending)
    try:
        while True:
            for event in events:
                if event.startswith('bye:'):
                    return 0, event[4:]
                if event == 'prompt':
                    waiting = False
            now = time.monotonic()
            if exiting and now >= exit_deadline:
                return 0, 'exit'
            if waiting and now >= deadline:
                waiting = False
            if not waiting and not exiting:
                if commands:
                    send(commands.pop(0))
                    waiting = True
                    deadline = now + 10
                elif args.cmd:
                    send('exit')
                    exiting = True
                    exit_deadline = now + 2
                else:
                    try:
                        line = lines.get_nowait()
                    except queue.Empty:
                        line = False
                    if line is None or line == 'exit':
                        send('exit')
                        exiting = True
                        exit_deadline = now + 2
                    elif line is not False:
                        send(line)
            if now >= next_ping and not exiting:
                send('#ping')
                next_ping = now + 10
            try:
                data = sock.recv(4096)
            except socket.timeout:
                events = []
                continue
            if not data:
                return 0, 'exit' if exiting else 'disconnect'
            events = output.feed(data)
    except KeyboardInterrupt:
        try:
            send('exit')
        except OSError:
            pass
        return 0, 'exit'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device')
    parser.add_argument('--password')
    parser.add_argument('--config', default=str(Path(__file__).resolve().parent.parent / 'config/device.conf'))
    parser.add_argument('--hello-port', type=int, default=5515)
    parser.add_argument('--once', action='store_true')
    parser.add_argument('--no-logs', action='store_true')
    parser.add_argument('--cmd', action='append')
    parser.add_argument('--list', type=float, nargs='?', const=10, dest='list_seconds')
    parser.add_argument('--bind-ip')
    parser.add_argument('--timeout', type=float)
    args = parser.parse_args(argv)
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(newline='', write_through=True)
    if args.cmd and any('\n' in c or '\r' in c for c in args.cmd):
        parser.error('Commands must be single lines')
    try:
        password = None if args.list_seconds is not None else password_for(args)
        lines = queue.Queue()
        if not args.cmd and args.list_seconds is None:
            threading.Thread(target=stdin_reader, args=(lines,), daemon=True).start()
        with udp_socket(args) as udp:
            status('Waiting for hello')
            timeout = args.timeout if args.timeout is not None else (3600 if args.cmd else None)
            end = time.monotonic() + args.list_seconds if args.list_seconds is not None else None
            wait_start = time.monotonic()
            selected = args.device.casefold() if args.device else None
            seen = {}
            if end is not None:
                print('SSID\tNAME\tIP\tFW', flush=True)
            while True:
                now = time.monotonic()
                if end is not None and now >= end:
                    return 0
                if end is None and timeout is not None and now - wait_start >= timeout:
                    status('No hello before timeout')
                    return 3
                try:
                    data, _ = udp.recvfrom(65535)
                except socket.timeout:
                    continue
                hello = parse_hello(data)
                if not hello:
                    continue
                if selected and selected not in (hello['ssid'].casefold(), hello['name'].casefold()):
                    continue
                key = (hello['ssid'], hello['ip'], hello['port'])
                if end is not None:
                    if key not in seen:
                        print('\t'.join(hello[k] for k in ('ssid', 'name', 'ip', 'fw')), flush=True)
                        seen[key] = now
                    continue
                # Suppress copies from multiple interfaces, but allow later wake cycles.
                if now - seen.get(key, -float('inf')) < 0.1:
                    continue
                seen[key] = now
                try:
                    # AF_INET + validated numeric IPv4 prevents DNS lookups.
                    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as tcp:
                        tcp.settimeout(max(0.05, min(2, hello['window_ms'] / 1000)))
                        tcp.connect((hello['ip'], hello['port']))
                        if selected is None:
                            selected = hello['ssid'].casefold()
                        status('Connected to ' + hello['ssid'] + ' (' + hello['ip'] + ')')
                        try:
                            code, reason = session(tcp, password, args, lines)
                        except KeyboardInterrupt:
                            try:
                                tcp.sendall(b'exit\n')
                            except OSError:
                                pass
                            return 0
                    if code or reason == 'exit' or args.once or args.cmd:
                        return code
                except OSError:
                    status('Connection lost or unavailable')
                    if args.once:
                        return 1
                wait_start = time.monotonic()
                status('Reconnecting; waiting for hello')
    except KeyboardInterrupt:
        return 0
    except (OSError, ValueError, EOFError):
        status('Unable to open console or read password/configuration')
        return 1


if __name__ == '__main__':
    sys.exit(main())
