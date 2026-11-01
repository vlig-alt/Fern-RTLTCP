#!/usr/bin/env python3
# Integration test for the rtltcp module: drives the binary against a fake
# rtl_tcp server and checks the exact bytes it sends, for control=true and
# control=false, plus --describe/--list-devices. SPDX-License-Identifier: GPL-2.0-or-later

import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time

BINARY = sys.argv[1] if len(sys.argv) > 1 else 'build/rtltcp'

CMD = {1: 'set_freq', 2: 'set_rate', 3: 'gain_mode', 4: 'set_gain', 5: 'set_ppm',
       14: 'bias_tee'}


class FakeRtlTcp:
    def __init__(self, port):
        self.commands = []
        self.accepted = False
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(('127.0.0.1', port))
        self.srv.listen(2)
        self.conn = None
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        try:
            self.conn, _ = self.srv.accept()
            self.accepted = True
            self.conn.sendall(b'RTL0' + struct.pack('>I', 6) + struct.pack('>I', 29))
            threading.Thread(target=self._stream, daemon=True).start()
            buf = b''
            while True:
                try:
                    d = self.conn.recv(1024)
                except OSError:
                    break
                if not d:
                    break
                buf += d
                while len(buf) >= 5:
                    self.commands.append((buf[0], struct.unpack('>I', buf[1:5])[0]))
                    buf = buf[5:]
        except Exception:
            pass

    def _stream(self):
        try:
            payload = bytes(range(256)) * 256
            while True:
                self.conn.sendall(payload)
                time.sleep(0.05)
        except Exception:
            pass

    def stop(self):
        try:
            if self.conn:
                self.conn.close()
            self.srv.close()
        except Exception:
            pass


def run_case(label, control, port, do_set):
    # Open the events file first so it takes fd 3; the fake server's socket
    # then gets a higher fd and is not clobbered by the dup2 below.
    events_path = f'/tmp/ev_{port}.log'
    efd = os.open(events_path, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
    if efd != 3:
        os.dup2(efd, 3)
        os.close(efd)
    fake = FakeRtlTcp(port)
    p = subprocess.Popen([BINARY, '--fernsdr-module', '1'],
                         stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                         pass_fds=(3,), close_fds=True)

    settings = {'host': '127.0.0.1', 'port': port, 'device': 't', 'control': control,
                'gain': 'auto', 'ppm': 0, 'bias_tee': False}

    def send(obj):
        p.stdin.write((json.dumps(obj) + '\n').encode())
        p.stdin.flush()

    send({'type': 'open', 'sample_rate': 2048000, 'center': 101000000, 'settings': settings})
    time.sleep(1.5)
    if do_set:
        send({'type': 'set', 'id': 7, 'settings': {'gain': '25.4'}})
        time.sleep(0.6)
    send({'type': 'stop'})
    try:
        rc = p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()
        rc = p.wait()
    time.sleep(0.3)
    fake.stop()
    stderr = p.stderr.read().decode(errors='replace') if p.stderr else ''
    events = []
    with open(events_path) as f:
        for line in f:
            line = line.strip()
            if line:
                events.append(json.loads(line))
    os.unlink(events_path)
    types = [e.get('type') for e in events]
    return rc, types, fake.commands, events, stderr


def expect(condition, message):
    if not condition:
        print('FAIL: ' + message)
        sys.exit(1)


def main():
    # --describe
    out = subprocess.run([BINARY, '--describe'], capture_output=True, text=True, timeout=10)
    expect(out.returncode == 0, '--describe exit status ' + str(out.returncode))
    d = json.loads(out.stdout)
    expect(d['id'] == 'rtltcp', '--describe id is ' + str(d.get('id')))
    expect(d['kind'] == 'input' and d['api'] == 1, '--describe kind/api')
    expect(isinstance(d['settings'], list) and len(d['settings']) == 7, '--describe settings count')
    expect('tuning' in d and d['tuning']['signal'] == 'iq', '--describe tuning')
    print('PASS --describe: id=%s version=%s settings=%d' % (d['id'], d['version'], len(d['settings'])))

    # --list-devices
    out = subprocess.run([BINARY, '--list-devices'], capture_output=True, text=True, timeout=10)
    expect(out.returncode == 0, '--list-devices exit status ' + str(out.returncode))
    ld = json.loads(out.stdout)
    expect('devices' in ld, '--list-devices has devices')
    print('PASS --list-devices: ' + str(ld['devices']))

    # control=true
    rc, types, cmds, events, stderr = run_case('CONTROL TRUE', True, 23458, do_set=True)
    print('control=true rc=%s types=%s stderr=%r' % (rc, types, stderr[:200]))
    expect(rc == 0, 'control=true exit status ' + str(rc))
    expect('hello' in types and 'ready' in types, 'control=true hello/ready, got ' + str(types))
    expect('applied' in types, 'control=true applied, got ' + str(types))
    expected = [(2, 2048000), (1, 101000000), (3, 0), (5, 0), (14, 0),  # open
                (3, 1), (4, 254), (5, 0), (14, 0)]                        # set gain=25.4
    expect(cmds == expected, 'control=true commands mismatch:\n  got ' + str(cmds) + '\n  want ' + str(expected))
    # ready reports the requested center/rate and the R828D tuner from the header
    ready = next((e for e in events if e.get('type') == 'ready'), None)
    expect(ready and ready.get('center') == 101000000 and ready.get('sample_rate') == 2048000,
           'control=true ready center/rate, got ' + str(ready))
    expect(ready and (ready.get('device') or {}).get('tuner') == 'R828D', 'control=true ready tuner')
    print('PASS control=true: ' + str([(CMD.get(c, c), v) for c, v in cmds]))

    # control=false
    rc, types, cmds, events, stderr = run_case('CONTROL FALSE', False, 23459, do_set=True)
    print('control=false rc=%s types=%s stderr=%r' % (rc, types, stderr[:200]))
    expect(rc == 0, 'control=false exit status ' + str(rc))
    expect('hello' in types and 'ready' in types, 'control=false hello/ready, got ' + str(types))
    expect(cmds == [], 'control=false must send nothing, got ' + str(cmds))
    print('PASS control=false: no commands sent')

    print('ALL TESTS PASSED')


if __name__ == '__main__':
    main()
