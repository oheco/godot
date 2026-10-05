#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Compile/sign/run the actual broker helper against loopback protocol fixtures.

Requires native OpenHarmony, native SDK and binary-sign-tool. Temporary endpoint
files, binaries and listener sockets are owned and cleaned by this runner. No HDC
or broker service is started. An optional --real-endpoint/--hdc performs read-only
list targets through an already-running managed broker service. Evidence contains
build commands, source hashes and individual fixture results, never binaries.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading

# Import the shared SDK locator without leaving generated files in the checkout.
sys.dont_write_bytecode = True
from test_process_execution import native_sdk

MAGIC = b"OHECOB1\n"


def exact(connection, count):
    result = bytearray()
    while len(result) < count:
        data = connection.recv(count - len(result))
        if not data:
            raise EOFError("Peer closed protocol stream")
        result.extend(data)
    return bytes(result)


def frame(kind, payload=b""):
    return bytes([kind]) + struct.pack("!I", len(payload)) + payload


def receive(connection):
    header = exact(connection, 5)
    size, = struct.unpack("!I", header[1:])
    assert size <= 1048576, "Oversized client frame"
    return header[0], exact(connection, size)


def exit_frame(code=0, reason=0, signal=0):
    return frame(8, struct.pack("!III", reason, code, signal))


def decode_start(payload):
    offset = 0

    def integer():
        nonlocal offset
        value, = struct.unpack("!I", payload[offset:offset + 4])
        offset += 4
        return value

    def text():
        nonlocal offset
        size = integer()
        value = payload[offset:offset + size].decode("utf-8", "strict")
        assert "\0" not in value
        offset += size
        return value

    command, cwd = text(), text()
    args = [text() for _ in range(integer())]
    env = [(text(), text()) for _ in range(integer())]
    stdin = payload[offset]
    assert offset + 1 == len(payload)
    return {"command": command, "cwd": cwd, "args": args, "env": env, "stdin": stdin}


class Fixture:
    def __init__(self, directory, name, handler, expected, handshake=None, ack=True):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.listener.settimeout(8)
        self.endpoint = directory / (name + ".endpoint")
        self.endpoint.write_text("127.0.0.1:" + str(self.listener.getsockname()[1]) + "\n")
        self.count = 0
        self.failure = None
        self.cancelled = False

        def serve():
            try:
                with self.listener.accept()[0] as connection:
                    self.count += 1
                    connection.settimeout(7)
                    assert exact(connection, 8) == MAGIC
                    if handshake:
                        handshake(connection)
                    else:
                        connection.sendall(MAGIC)
                    if handshake is not wrong_magic:
                        kind, payload = receive(connection)
                        assert kind == 1
                        start = decode_start(payload)
                        assert start == {"command": expected[0], "cwd": "", "args": expected[1], "env": [], "stdin": 0}, start
                        if ack:
                            connection.sendall(frame(2))
                        handler(connection, self)
                    assert connection.recv(1) == b"", "No-stdin command sent unexpected client data"
                # Any reconnect is a failed no-retry assertion, including a
                # second START after unknown outcome or ordinary nonzero exit.
                self.listener.settimeout(0.2)
                try:
                    extra, _ = self.listener.accept()
                except socket.timeout:
                    pass
                else:
                    self.count += 1
                    extra.close()
                    raise AssertionError("Client retried a broker request")
            except BaseException as error:
                self.failure = error
            finally:
                self.listener.close()

        self.thread = threading.Thread(target=serve, name="broker-protocol-fixture", daemon=True)
        self.thread.start()

    def finish(self):
        self.thread.join(9)
        assert not self.thread.is_alive(), "Protocol fixture did not clean up"
        if self.failure:
            raise self.failure
        assert self.count == 1


def cancel_or_disconnect(connection, fixture):
    try:
        kind, payload = receive(connection)
        assert kind == 7 and not payload, (kind, len(payload))
        fixture.cancelled = True
        assert connection.recv(1) == b"", "Cancelled managed handle was not released"
    except (EOFError, ConnectionResetError):
        fixture.cancelled = True


def fragmented(connection, data, width=2, delay=0.002):
    for offset in range(0, len(data), width):
        connection.sendall(data[offset:offset + width])
        threading.Event().wait(delay)


def wrong_magic(connection):
    connection.sendall(b"BADMAGC\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", default=os.environ.get("OPENHARMONY_SDK_PATH"))
    parser.add_argument("--cxx", default=os.environ.get("CXX"))
    parser.add_argument("--cc", default=os.environ.get("CC"))
    parser.add_argument("--sign-tool", default=shutil.which("binary-sign-tool"))
    parser.add_argument("--output", type=Path, help="Fresh evidence directory")
    parser.add_argument("--real-endpoint", type=Path, help="Existing service endpoint for read-only HDC smoke")
    parser.add_argument("--hdc", type=Path, help="Absolute existing HDC executable, required with --real-endpoint")
    args = parser.parse_args()
    if sys.platform != "ohos" or not args.sdk or not args.sign_tool:
        parser.error("Run on native OpenHarmony with --sdk and binary-sign-tool")
    if bool(args.real_endpoint) != bool(args.hdc):
        parser.error("Pass both --real-endpoint and --hdc, or neither")
    tmpdir = os.environ.get("TMPDIR")
    if not tmpdir or not Path(tmpdir).is_dir():
        parser.error("TMPDIR must name an existing writable directory")
    sdk = native_sdk(args.sdk)
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
    tests = Path(__file__).resolve().parent
    root = tests.parents[2]
    source = tests / "native_broker_fixture/client.cpp"
    helper = tests.parent / "export/broker_client.h"
    vendor = root / "thirdparty/oheco-broker"
    cc = args.cc or str(sdk / "llvm/bin/clang")
    cxx = args.cxx or str(sdk / "llvm/bin/clang++")
    builds, results = [], []
    report = {"platform": sys.platform, "nativeSdk": str(sdk), "sourceSha256": {
        str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in (source, helper, Path(__file__).resolve(), root / "editor/export/SCsub", vendor / "oheco_broker.c", vendor / "oheco_broker.h", vendor / "LICENSE", vendor / "README.godot.md")},
        "scope": "No-stdin managed commands through the unchanged SDK; native signed CLI against fake v1 protocol servers and optional read-only real HDC discovery. No file upload, Godot build, HAP, UIAbility or installation.",
        "commandStdinEnabled": False, "brokerControlConnectionFileBytes": 0,
        "results": results}
    try:
        with tempfile.TemporaryDirectory(prefix="godot-broker-client-", dir=tmpdir) as temporary:
            directory = Path(temporary)
            obj = directory / "broker.o"
            unsigned = directory / "client.unsigned"
            executable = directory / "native broker 'client'"
            common = ["--target=aarch64-linux-ohos", "--sysroot=" + str(sdk / "sysroot"), "-Wall", "-Wextra", "-Werror", "-pthread"]
            commands = [[cc, *common, "-std=c11", "-c", str(vendor / "oheco_broker.c"), "-o", str(obj)],
                        [cxx, *common, "-std=c++17", "-fno-exceptions", "-fno-rtti", "-I" + str(tests.parent), str(source), str(obj), "-o", str(unsigned)],
                        [args.sign_tool, "sign", "-selfSign", "1", "-inFile", str(unsigned), "-outFile", str(executable)]]
            for command in commands:
                completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
                builds.append({"command": command, "exitCode": completed.returncode,
                               "output": completed.stdout.decode("utf-8", "replace")})
                if completed.returncode:
                    raise RuntimeError("Native compile/sign failed:\n" + builds[-1]["output"])
            executable.chmod(0o700)
            report["signedExecutableSha256"] = hashlib.sha256(executable.read_bytes()).hexdigest()
            environment = dict(os.environ)
            environment.pop("LD_PRELOAD", None)
            environment.pop("LD_LIBRARY_PATH", None)

            def run(endpoint, mode="call", timeout=10000, read_stderr=True, command="/fixture/hdc", argv=None):
                native = subprocess.run([str(executable), mode, str(endpoint), str(timeout), "1" if read_stderr else "0", command, *(argv or [])],
                                        env=environment, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
                assert native.returncode == 0, native.stderr.decode("utf-8", "replace")
                pieces = native.stdout.split(b"\n", 5)
                assert len(pieces) == 6
                error, code, elapsed, size, diagnostic_size = (int(value) for value in pieces[:5])
                assert len(pieces[5]) == size + diagnostic_size
                return {"error": error, "exitCode": code, "elapsedMs": elapsed,
                        "output": pieces[5][:size], "diagnostic": pieces[5][size:].decode("utf-8", "strict")}

            def check(name, handler, expected_error=0, code=0, expected_output=None, mode="call", timeout=10000,
                      read_stderr=True, argv=None, handshake=None, ack=True, max_elapsed=None):
                fixture = Fixture(directory, name, handler, ("/fixture/hdc", argv or []),
                                  handshake=handshake, ack=ack)
                result = run(fixture.endpoint, mode=mode, timeout=timeout, read_stderr=read_stderr, argv=argv)
                try:
                    fixture.finish()
                except BaseException:
                    results.append({"name": name, "passed": False, **{key: value for key, value in result.items() if key != "output"},
                                    "outputBytes": len(result["output"]), "requestCount": fixture.count})
                    raise
                assert result["error"] == expected_error, result
                assert result["exitCode"] == code, result
                if expected_output is not None:
                    assert result["output"] == expected_output, {**result, "output": result["output"][:100]}
                if max_elapsed:
                    assert result["elapsedMs"] < max_elapsed, result
                assert not result["error"] or result["diagnostic"], "Failure has no diagnostic"
                results.append({"name": name, "passed": True, "requestCount": fixture.count, "cancelledOrDisconnected": fixture.cancelled,
                                **{key: value for key, value in result.items() if key != "output"},
                                "outputBytes": len(result["output"]), "outputSha256": hashlib.sha256(result["output"]).hexdigest()})
                print("PASS: " + name, flush=True)

            def fragments(connection, _):
                fragmented(connection, frame(5, "fragment中文\n".encode()), width=1, delay=0.008)
                fragmented(connection, frame(6, b"stderr\n"), width=1)
                fragmented(connection, exit_frame(), width=1)
            check("fragmented-header-payload-across-read-timeouts", fragments, expected_output="fragment中文\nstderr\n".encode(),
                  handshake=lambda connection: fragmented(connection, MAGIC, width=1))
            check("nonzero-exit", lambda connection, _: connection.sendall(exit_frame(37)), code=37)
            check("signal-exit", lambda connection, _: connection.sendall(exit_frame(0xffffffff, 1, 15)), code=143)
            check("cancelled-exit-is-error", lambda connection, _: connection.sendall(exit_frame(0, 2)), expected_error=7, code=-1)
            quoted = ["", "two words", "single'quote", 'double"quote', "$(touch sentinel); & |", "中文路径.hap"]
            check("quoted-argv-preserved-without-shell", lambda connection, _: connection.sendall(exit_frame()), argv=quoted)

            stdout_block, stderr_block = b"O" * 65536, b"E" * 65536
            def streams(connection, _):
                for _ in range(32):
                    connection.sendall(frame(5, stdout_block) + frame(6, stderr_block))
                connection.sendall(exit_frame())
            check("continuous-stdout-stderr-drain-4MiB", streams, expected_output=(stdout_block + stderr_block) * 32)
            check("stderr-filter-still-drains", streams, expected_output=stdout_block * 32, read_stderr=False)

            def partial_timeout(connection, fixture):
                connection.sendall(frame(5, b"incomplete")[:7])
                cancel_or_disconnect(connection, fixture)
            check("monotonic-total-deadline-cancels-partial-frame", partial_timeout, expected_error=6, code=-1, timeout=120, max_elapsed=1000)

            def late_start(connection, fixture):
                threading.Event().wait(0.3)
                try:
                    connection.sendall(frame(2))
                    assert connection.recv(1) == b""
                except (BrokenPipeError, ConnectionResetError):
                    pass
                fixture.cancelled = True
            check("startup-included-in-total-deadline", late_start, expected_error=6, code=-1, mode="startup-keepalive", ack=False, timeout=100, max_elapsed=700)
            check("disconnect-after-ACK-no-retry", lambda connection, _: connection.shutdown(socket.SHUT_RDWR), expected_error=4, code=-1)
            check("disconnect-after-START-before-ACK-no-retry", lambda connection, _: connection.shutdown(socket.SHUT_RDWR), expected_error=4, code=-1, ack=False)
            check("invalid-EXIT-is-protocol-error", lambda connection, fixture: (connection.sendall(exit_frame(300)), cancel_or_disconnect(connection, fixture)), expected_error=2, code=-1)
            check("oversized-frame-is-protocol-error", lambda connection, fixture: (connection.sendall(bytes([5]) + struct.pack("!I", 1048577)), cancel_or_disconnect(connection, fixture)), expected_error=2, code=-1)
            check("invalid-handshake-no-retry", lambda connection, _: None, expected_error=2, code=-1, handshake=wrong_magic)
            message = b"fixture spawn failed"
            check("server-spawn-error-not-success", lambda connection, _: connection.sendall(frame(9, struct.pack("!II", 3, len(message)) + message)), expected_error=3, code=-1, ack=False)

            def limit(connection, fixture):
                try:
                    for _ in range(1025):
                        connection.sendall(frame(6, stderr_block))
                    cancel_or_disconnect(connection, fixture)
                except (BrokenPipeError, ConnectionResetError):
                    fixture.cancelled = True
            check("64MiB-limit-includes-discarded-stderr", limit, expected_error=8, code=-1, read_stderr=False, expected_output=b"")
            for mode in ("nul-command", "nul-arg", "bad-utf8"):
                result = run(directory / "does-not-exist", mode=mode)
                assert result["error"] == 5 and result["exitCode"] == -1, result
                results.append({"name": mode + "-rejected-before-connect", "passed": True, **{k: v for k, v in result.items() if k != "output"}})
                print("PASS: " + results[-1]["name"], flush=True)
            result = run(directory / "does-not-exist", timeout=0)
            assert result["error"] == 5 and result["exitCode"] == -1
            results.append({"name": "nonpositive-timeout-rejected", "passed": True})
            if args.real_endpoint:
                real = run(args.real_endpoint, timeout=120000, command=str(args.hdc.resolve()), argv=["list", "targets"])
                assert real["error"] == 0 and real["exitCode"] == 0, real
                results.append({"name": "real-shell-serve-hdc-list-targets", "passed": True, "endpoint": str(args.real_endpoint),
                                "hdc": str(args.hdc.resolve()), **{k: v for k, v in real.items() if k != "output"},
                                "output": real["output"].decode("utf-8", "replace")})
                print("PASS: real-shell-serve-hdc-list-targets", flush=True)
            report["passed"] = True
            print("OHOS_BROKER_CLIENT_PASS", flush=True)
    except BaseException as error:
        report["passed"] = False
        report["failure"] = repr(error)
        raise
    finally:
        if args.output:
            (args.output / "build.json").write_text(json.dumps(builds, indent=2, ensure_ascii=False) + "\n")
            (args.output / "result.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    main()
