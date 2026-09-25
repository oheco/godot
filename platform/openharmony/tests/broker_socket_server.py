#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Ping-only abstract Unix socket server; never executes commands.

Run in the command-line environment before launching the editor. The editor
shell connects to @oheco.broker.v1 and records the result in its diagnostics log.
This is a connectivity probe, NOT an authenticated command execution broker.
"""
import argparse
import logging
import os
from pathlib import Path
import signal
import socket
import struct
import time

REQUEST = b"OHECO_BROKER_PROBE_V1\n"
RESPONSE = b"OHECO_BROKER_PROBE_OK_V1\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--socket-name', default='oheco.broker.v1')
    parser.add_argument('--log-file', type=Path)
    args = parser.parse_args()
    name = args.socket_name.encode('utf-8')
    if not name or b'\0' in name or len(name) > 90:
        parser.error('socket name must contain 1..90 bytes and no NUL')
    handlers = [logging.StreamHandler()]
    if args.log_file:
        # Append, so restarting this test never erases earlier evidence.
        handlers.append(logging.FileHandler(args.log_file, encoding='utf-8'))
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(message)s', handlers=handlers)
    log = logging.getLogger('broker-probe')
    try:
        domain = Path('/proc/self/attr/current').read_bytes().rstrip(b'\0\n').decode('utf-8', 'replace')
    except OSError as exc:
        domain = f'<unavailable errno={exc.errno}>'
    try:
        netns = os.readlink('/proc/self/ns/net')
    except OSError as exc:
        netns = f'<unavailable errno={exc.errno}>'

    def stop(_signum, _frame):
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        # No trailing NUL: the editor must use the exact same address length.
        listener.bind(b'\0' + name)
        listener.listen(8)
        log.info('LISTEN address=@%s pid=%d uid=%d gid=%d domain=%s netns=%s mode=ping-only',
                 args.socket_name, os.getpid(), os.getuid(), os.getgid(), domain, netns)
        try:
            while True:
                connection, _ = listener.accept()
                with connection:
                    peer = '<unavailable>'
                    try:
                        credentials = connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
                        pid, uid, gid = struct.unpack('3i', credentials)
                        peer = f'pid={pid} uid={uid} gid={gid}'
                        log.info('ACCEPT %s', peer)
                        deadline = time.monotonic() + 2.0
                        request = bytearray()
                        while b'\n' not in request and len(request) < 64:
                            remaining = deadline - time.monotonic()
                            if remaining <= 0:
                                raise TimeoutError('request deadline exceeded')
                            connection.settimeout(remaining)
                            data = connection.recv(64 - len(request))
                            if not data:
                                break
                            request.extend(data)
                        connection.settimeout(2.0)
                        if request != REQUEST:
                            log.warning('REJECT invalid probe frame bytes=%d %s', len(request), peer)
                            connection.sendall(b'ERROR protocol\n')
                            continue
                        connection.sendall(RESPONSE)
                        log.info('PING/PONG PASS %s', peer)
                    except OSError as exc:
                        log.warning('CLIENT_ERROR %s errno=%s message=%s', peer, exc.errno, exc)
        finally:
            log.info('STOP address=@%s', args.socket_name)


if __name__ == '__main__':
    main()
