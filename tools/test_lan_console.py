"""Loopback integration tests for the standalone console tools."""
from pathlib import Path
import socket
import subprocess
import sys
import time
import unittest

import lan_console as console

TOOLS = Path(__file__).resolve().parent


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class ConsoleTests(unittest.TestCase):
    def setUp(self):
        self.processes = []
        if self._testMethodName not in ("test_parse_hello", "test_pong_hidden",
                                           "test_stream_fragmentation", "test_session_without_network"):
            self.udp = free_port(socket.SOCK_DGRAM)
            self.tcp = free_port(socket.SOCK_STREAM)

    def tearDown(self):
        for proc in self.processes:
            if proc.poll() is None:
                proc.terminate()
            proc.communicate(timeout=5)

    def client(self, *args):
        proc = subprocess.Popen([sys.executable, str(TOOLS / 'lan_console.py'),
                                 '--hello-port', str(self.udp), '--bind-ip', '127.0.0.1',
                                 '--password', 'test', '--timeout', '3'] + list(args),
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
        self.processes.append(proc)
        return proc

    def device(self, *args):
        proc = subprocess.Popen([sys.executable, str(TOOLS / 'fake_device.py'),
                                 '--hello-port', str(self.udp), '--tcp-port', str(self.tcp),
                                 '--hello-addr', '127.0.0.1', '--bind-ip', '127.0.0.1',
                                 '--hello-every', '0.15'] + list(args),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.processes.append(proc)
        return proc

    def test_parse_hello(self):
        parsed = console.parse_hello(b'GM-HELLO v1 fw=1 name=My%20device ip=127.0.0.1 ssid=GM%2B1 window_ms=500 extra=yes\n')
        self.assertEqual(parsed['name'], 'My device')
        self.assertEqual(parsed['ssid'], 'GM+1')
        self.assertEqual(parsed['port'], 5516)
        for junk in (b'junk\n', b'GM-HELLO v2\n', b'GM-HELLO v1 ip=host\n', b'\xff\n'):
            self.assertIsNone(console.parse_hello(junk))

    def test_command_status(self):
        client = self.client('--cmd', 'status', '--cmd', 'echo done', '--no-logs')
        self.device()
        out, err = client.communicate(timeout=8)
        self.assertEqual(client.returncode, 0, err)
        self.assertIn('"status": "ok"', out)
        self.assertIn('done\r\n'.replace('\r', ''), out)
        self.assertIn('> ', out)
        self.assertIn('BYE exit', err)

    def test_auth_fail(self):
        client = self.client('--cmd', 'status')
        self.device('--password', 'other')
        out, err = client.communicate(timeout=8)
        self.assertEqual(client.returncode, 2, err)
        self.assertNotIn('other', out + err)

    def test_filter(self):
        client = self.client('--device', 'missing', '--cmd', 'status', '--timeout', '0.6')
        self.device()
        _, err = client.communicate(timeout=5)
        self.assertEqual(client.returncode, 3, err)

    def test_filter_case_insensitive_name(self):
        client = self.client('--device', 'FAKE GRAINMETER', '--cmd', 'status')
        self.device()
        out, err = client.communicate(timeout=8)
        self.assertEqual(client.returncode, 0, err)
        self.assertIn('"status": "ok"', out)

    def test_restart_reconnect(self):
        client = self.client()
        self.device()
        time.sleep(0.6)
        client.stdin.write('restart\n')
        client.stdin.flush()
        time.sleep(2.8)
        out, err = client.communicate('status\nexit\n', timeout=8)
        self.assertEqual(client.returncode, 0, err)
        self.assertGreaterEqual(err.count('Connected to'), 2, err)
        self.assertIn('"status": "ok"', out)

    def test_pong_hidden(self):
        output = console.Output()
        import contextlib
        import io
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            for chunk in (b'#po', b'ng\r', b'\nhello\r\n>', b' '):
                output.feed(chunk)
        self.assertEqual(stream.getvalue(), 'hello\r\n> ')

    def test_heartbeat(self):
        client = self.client()
        self.device('--idle-s', '11', '--log-every', '0.3')
        time.sleep(11.5)
        out, err = client.communicate('status\nexit\n', timeout=5)
        self.assertEqual(client.returncode, 0, err)
        self.assertIn('"status": "ok"', out)
        self.assertNotIn('#pong', out)
        self.assertNotIn('BYE idle', err)

    def test_stream_fragmentation(self):
        import contextlib
        import io
        wire = b'normal\r\n> #pong\r\nnext\r\n> BYE restart\r\n'
        for split in range(len(wire) + 1):
            output = console.Output()
            stream = io.StringIO()
            with contextlib.redirect_stdout(stream), contextlib.redirect_stderr(io.StringIO()):
                events = output.feed(wire[:split]) + output.feed(wire[split:])
            self.assertEqual(stream.getvalue(), 'normal\r\n> next\r\n> ')
            self.assertEqual(events, ['prompt', 'prompt', 'bye:restart'])

    def test_session_without_network(self):
        import argparse
        import contextlib
        import io
        import queue

        class Peer:
            def __init__(self):
                self.incoming = [b'GM-CONSOLE v1 ssid=GM fw=1\r\nAUTH?\r\n']
                self.sent = []

            def settimeout(self, timeout):
                pass

            def sendall(self, data):
                self.sent.append(data)
                replies = {b'auth secret\n': b'AUTH OK\r\n',
                           b'status\n': b'{"status": "ok"}\r\n> ',
                           b'exit\n': b'BYE exit\r\n'}
                self.incoming.append(replies[data])

            def recv(self, size):
                return self.incoming.pop(0)

        peer = Peer()
        stream = io.StringIO()
        args = argparse.Namespace(cmd=['status'], no_logs=False)
        with contextlib.redirect_stdout(stream), contextlib.redirect_stderr(io.StringIO()):
            result = console.session(peer, 'secret', args, queue.Queue())
        self.assertEqual(result, (0, 'exit'))
        self.assertEqual(peer.sent, [b'auth secret\n', b'status\n', b'exit\n'])
        self.assertEqual(stream.getvalue(), '{"status": "ok"}\r\n> ')

    def test_deduplicate_list(self):
        client = self.client('--list', '0.8')
        self.device('--hello-every', '0.05')
        out, err = client.communicate(timeout=5)
        self.assertEqual(client.returncode, 0, err)
        self.assertEqual(out.count('GrainMeter-0079'), 1)


if __name__ == '__main__':
    unittest.main()
