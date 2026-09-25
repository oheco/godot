#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Loopback TCP discovery + ping probe. NEVER executes commands.

The shared endpoint file, instance ID and echoed nonce are NOT authentication.
They only let this test detect a stale descriptor and correlate both logs.
"""
import argparse
import json
import logging
import os
from pathlib import Path
import re
import signal
import socket
import time
import uuid

REQUEST = b'OHECO_BROKER_TCP_PROBE_V1'
RESPONSE = b'OHECO_BROKER_TCP_PROBE_OK_V1'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--endpoint-file', type=Path,
                        default=Path('/storage/Users/currentUser/.oheco/broker/endpoint.json'))
    parser.add_argument('--log-file', type=Path)
    args = parser.parse_args()
    handlers = [logging.StreamHandler()]
    if args.log_file:
        handlers.append(logging.FileHandler(args.log_file, encoding='utf-8'))
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(message)s', handlers=handlers)
    log = logging.getLogger('broker-tcp-probe')
    endpoint = args.endpoint_file
    endpoint.parent.mkdir(parents=True, exist_ok=True)
    # This small test server does not take over another service's discovery file.
    if os.path.lexists(endpoint):
        raise SystemExit(f'Refusing to replace existing discovery file: {endpoint}')
    instance = uuid.uuid4().hex
    # Same-directory staging is necessary for atomic publication on this filesystem.
    staged = endpoint.with_name(f'.endpoint-{instance}.json')
    published = False
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
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(8)
            host, port = listener.getsockname()
            descriptor = {
                'protocolVersion': 1, 'transport': 'tcp', 'mode': 'ping-only',
                'host': host, 'port': port, 'instanceId': instance, 'pid': os.getpid(),
            }
            with staged.open('x', encoding='utf-8') as file:
                json.dump(descriptor, file, indent=2)
                file.write('\n')
            os.replace(staged, endpoint)
            published = True
            log.info('LISTEN address=%s:%d pid=%d uid=%d domain=%s netns=%s instance=%s mode=ping-only',
                     host, port, os.getpid(), os.getuid(), domain, netns, instance)
            log.info('DISCOVERY file=%s', endpoint)
            while True:
                connection, peer = listener.accept()
                with connection:
                    log.info('ACCEPT peer=%s:%d (TCP does not provide peer UID)', *peer)
                    try:
                        deadline = time.monotonic() + 2.0
                        request = bytearray()
                        while b'\n' not in request and len(request) < 256:
                            remaining = deadline - time.monotonic()
                            if remaining <= 0:
                                raise TimeoutError('request deadline exceeded')
                            connection.settimeout(remaining)
                            data = connection.recv(256 - len(request))
                            if not data:
                                break
                            request.extend(data)
                        connection.settimeout(2.0)
                        parts = bytes(request).removesuffix(b'\n').split(b' ')
                        if (not request.endswith(b'\n') or len(parts) != 3 or parts[0] != REQUEST or
                                re.fullmatch(rb'[0-9-]{1,64}', parts[2]) is None):
                            log.warning('REJECT protocol peer=%s:%d', *peer)
                            connection.sendall(b'ERROR protocol\n')
                            continue
                        if parts[1] != instance.encode('ascii'):
                            log.warning('REJECT stale_instance peer=%s:%d', *peer)
                            connection.sendall(b'ERROR stale_instance\n')
                            continue
                        connection.sendall(RESPONSE + b' ' + parts[1] + b' ' + parts[2] + b'\n')
                        log.info('PING/PONG PASS peer=%s:%d instance=%s nonce=%s (nonce is client-supplied)',
                                 *peer, instance, parts[2].decode('ascii'))
                    except OSError as exc:
                        log.warning('CLIENT_ERROR peer=%s:%d errno=%s message=%s', *peer, exc.errno, exc)
    finally:
        staged.unlink(missing_ok=True)
        # A stale file is tolerable; deleting a different instance's file is not.
        if published:
            try:
                current = json.loads(endpoint.read_text(encoding='utf-8'))
                if current.get('instanceId') == instance:
                    endpoint.unlink()
                    log.info('Removed discovery file for instance=%s', instance)
            except (OSError, ValueError, AttributeError):
                log.warning('Discovery cleanup skipped; could not confirm ownership')
        log.info('STOP instance=%s', instance)


if __name__ == '__main__':
    main()
