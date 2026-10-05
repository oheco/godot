#!/usr/bin/env python3
# Godot Engine contributors. SPDX-License-Identifier: MIT
"""Native SDK -> existing broker -> real HDC command/path regression.

Every command has stdin disabled. Optional --shared-hap passes only a project
.godot HAP path so broker Node can stat/hash the existing shared file. HDC itself
reads and transfers --install-hap to its device; the broker control connection
carries no HAP bytes. Port tests require explicit --same-device-broker-node and
prove local target IP plus matching target/broker/listener network namespaces.
No external target Node is staged/executed, and no policy or service is changed.
Only a confirmed owned reverse pair is removed. Replacement installation is
explicit and guarded by an empty pidof; no application is launched or stopped.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import threading

sys.dont_write_bytecode = True
from test_process_execution import native_sdk

NAMESPACE_SCRIPT = r"""
const fs=require('fs');console.log(JSON.stringify({netns:fs.readlinkSync('/proc/self/ns/net')}));
"""
# These are trusted static programs. Dynamic values are individual argv entries,
# shell-quoted only because HDC shell joins remote arguments into a command.
PORT_SCRIPT = r"""
const net=require('net');const avoid=new Set(JSON.parse(process.argv[1]));let attempts=0;
function pick(){if(++attempts>32)throw Error('No unused port found');const s=net.createServer();
s.on('error',e=>{console.error(String(e));process.exit(1);});
s.listen(0,'127.0.0.1',()=>{const port=s.address().port;s.close(()=>{if(avoid.has(port))pick();else console.log(JSON.stringify({port}));});});}
pick();
"""
ECHO_SCRIPT = r"""
const net=require('net');const port=Number(process.argv[1]),hex=process.argv[2];
if(!Number.isInteger(port)||port<1||port>65535||!/^[0-9a-f]{64}$/.test(hex))process.exit(2);
const nonce=Buffer.from(hex,'hex'),expected=Buffer.concat([Buffer.from('godot-broker-echo:'),nonce]);
const s=net.connect({host:'127.0.0.1',port});let chunks=[],size=0;
function fail(e){console.error(String(e));s.destroy();process.exitCode=1;}
s.setTimeout(10000,()=>fail('Echo timeout'));s.on('error',fail);
s.on('connect',()=>s.write(nonce));s.on('data',b=>{size+=b.length;if(size>expected.length)fail('Unexpected echo length');else chunks.push(b);});
s.on('end',()=>{const data=Buffer.concat(chunks);if(!data.equals(expected))fail('Echo bytes differ');else console.log(JSON.stringify({port,nonce:hex,response:data.toString('hex'),size:data.length}));});
"""
SHARED_HAP_SCRIPT = r"""
const fs=require('fs'),path=require('path'),crypto=require('crypto');const hap=process.argv[1];
if(!path.isAbsolute(hap)||!hap.split(path.sep).includes('.godot'))process.exit(2);
const st=fs.statSync(hap);if(!st.isFile()||st.size<=0)process.exit(3);
const digest=crypto.createHash('sha256'),input=fs.createReadStream(hap);
input.on('data',b=>digest.update(b));input.on('error',e=>{console.error(String(e));process.exitCode=1;});
input.on('end',()=>console.log(JSON.stringify({hap,size:st.size,sha256:digest.digest('hex')})));
"""


class Echo:
    def __init__(self, nonce):
        self.nonce = nonce
        self.received = None
        self.failure = None
        self.stop = threading.Event()
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.thread = None

    def start(self):
        def serve():
            try:
                while not self.stop.is_set():
                    try:
                        connection, _ = self.listener.accept()
                    except socket.timeout:
                        continue
                    with connection:
                        connection.settimeout(5)
                        data = bytearray()
                        while len(data) < len(self.nonce):
                            part = connection.recv(len(self.nonce) - len(data))
                            if not part:
                                raise AssertionError("Echo client closed before sending the nonce")
                            data.extend(part)
                        self.received = bytes(data)
                        if self.received != self.nonce:
                            raise AssertionError("Host received different nonce bytes")
                        connection.sendall(b"godot-broker-echo:" + self.nonce)
                        connection.shutdown(socket.SHUT_WR)
                    return
            except BaseException as error:
                if not self.stop.is_set():
                    self.failure = error
        self.thread = threading.Thread(target=serve, name="owned-broker-device-echo", daemon=True)
        self.thread.start()

    def close(self):
        self.stop.set()
        self.listener.close()
        if self.thread:
            self.thread.join(6)
            if self.thread.is_alive():
                raise AssertionError("Owned echo thread did not finish")
        if self.failure:
            raise self.failure


def rules(text):
    return Counter(" ".join(line.split()) for line in text.splitlines()
                   if line.strip() and line.strip() not in ("Empty", "[Empty]"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", default=os.environ.get("OPENHARMONY_SDK_PATH"))
    parser.add_argument("--cc", default=os.environ.get("CC"))
    parser.add_argument("--cxx", default=os.environ.get("CXX"))
    parser.add_argument("--sign-tool", default=shutil.which("binary-sign-tool"))
    parser.add_argument("--endpoint", type=Path, default=Path.home() / ".oheco/broker/endpoint")
    parser.add_argument("--target", default="192.168.1.160:34835")
    parser.add_argument("--hdc", type=Path, default=Path.home() / ".oheco/packages/ohos-sdk-toolchains/26.0.0.35-Beta/hdc")
    parser.add_argument("--node", type=Path, default=Path.home() / ".oheco/packages/nodejs/24.21.0-ohos.1/bin/node")
    parser.add_argument("--same-device-broker-node", action="store_true", help="For self HDC only: verify target IPv4 is local and shares the network namespace, then use broker Node as the target-network TCP client")
    parser.add_argument("--output", type=Path, required=True, help="Fresh cache evidence directory")
    parser.add_argument("--shared-hap", type=Path, help="Existing HAP inside a shared project .godot directory; stat/hash only, no file transfer")
    parser.add_argument("--install-hap", type=Path, help="Explicitly authorized signed HAP replacement; no app launch")
    parser.add_argument("--editor-process", help="Exact pidof name required with --install-hap")
    parser.add_argument("--install-only", action="store_true", help="Only verify target/pidof and the explicitly authorized HAP installation; skip ports")
    args = parser.parse_args()
    if sys.platform != "ohos" or not args.sdk or not args.sign_tool:
        parser.error("Run on native OpenHarmony with --sdk and binary-sign-tool")
    if not args.install_only and not args.same_device_broker_node:
        parser.error("Port tests require explicit --same-device-broker-node; no target-shell fallback")
    if args.shared_hap and (not args.shared_hap.is_absolute() or '.godot' not in args.shared_hap.parts):
        parser.error("--shared-hap must be an absolute shared-project .godot path")
    if args.install_only and not args.install_hap:
        parser.error("--install-only requires --install-hap and --editor-process")
    if bool(args.install_hap) != bool(args.editor_process):
        parser.error("Pass both --install-hap and --editor-process, or neither")
    if args.editor_process and not re.fullmatch(r"[A-Za-z0-9_.-]+", args.editor_process):
        parser.error("editor-process must be a plain process name")
    tmpdir = os.environ.get("TMPDIR")
    if not tmpdir or not Path(tmpdir).is_dir():
        parser.error("TMPDIR must be an existing writable directory")
    for path in (args.endpoint, args.hdc, args.node, *([args.install_hap] if args.install_hap else []), *([args.shared_hap] if args.shared_hap else [])):
        if not path.is_file():
            parser.error("Required input does not exist: " + str(path))
    sdk = native_sdk(args.sdk)
    args.output.mkdir(parents=True, exist_ok=False)
    tests = Path(__file__).resolve().parent
    root = tests.parents[2]
    source = tests / "native_broker_fixture/client.cpp"
    helper = tests.parent / "export/broker_client.h"
    vendor = root / "thirdparty/oheco-broker"
    builds, calls, passed = [], [], []
    report = {"platform": sys.platform, "nativeSdk": str(sdk), "endpoint": str(args.endpoint), "target": args.target,
              "hdc": str(args.hdc), "node": str(args.node), "calls": calls, "checks": passed,
              "requestedMode": "install-only" if args.install_only else "ports-and-shared-path",
              "scope": "No-stdin command RPC; shared project path stat/hash and owned HDC reverse TCP byte roundtrip; optional authorized HDC install after empty pidof. No app launch/stop, service restart, external target Node execution or game debug-session acceptance.",
              "sourceSha256": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                               for path in (source, helper, Path(__file__).resolve(), vendor / "oheco_broker.c", vendor / "oheco_broker.h")},
              "exportSourceSha256": hashlib.sha256((tests.parent / "export/export_plugin.cpp").read_bytes()).hexdigest(),
               "sharedPathValidationScope": "Existing file visibility and bytes only; not proof of a game build or Run",
               "byteTransfer": {"brokerControlConnectionStdinEnabled": False,
                               "brokerControlConnectionHapBytes": 0,
                               "hdcDeviceInstallation": "HDC reads the passed file path and transfers package bytes using its own device transport"}}
    environment = dict(os.environ)
    environment.pop("LD_PRELOAD", None)
    environment.pop("LD_LIBRARY_PATH", None)
    owned_pair = None
    echo = None
    baseline = None
    cleanup_errors = []
    try:
        with tempfile.TemporaryDirectory(prefix="godot-broker-device-", dir=tmpdir) as temporary:
            directory = Path(temporary)
            obj, unsigned, executable = directory / "broker.o", directory / "client.unsigned", directory / "native broker device client"
            common = ["--target=aarch64-linux-ohos", "--sysroot=" + str(sdk / "sysroot"), "-Wall", "-Wextra", "-Werror", "-pthread"]
            commands = [[args.cc or str(sdk / "llvm/bin/clang"), *common, "-std=c11", "-c", str(vendor / "oheco_broker.c"), "-o", str(obj)],
                        [args.cxx or str(sdk / "llvm/bin/clang++"), *common, "-std=c++17", "-fno-exceptions", "-fno-rtti", "-I" + str(tests.parent), str(source), str(obj), "-o", str(unsigned)],
                        [args.sign_tool, "sign", "-selfSign", "1", "-inFile", str(unsigned), "-outFile", str(executable)]]
            for command in commands:
                completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
                builds.append({"command": command, "exitCode": completed.returncode, "output": completed.stdout.decode("utf-8", "replace")})
                if completed.returncode:
                    raise RuntimeError("Native build/sign failed: " + builds[-1]["output"])
            executable.chmod(0o700)
            report["signedExecutableSha256"] = hashlib.sha256(executable.read_bytes()).hexdigest()

            def rpc(label, command, argv, timeout=30000, allowed_codes=(0,), is_hdc=False):
                completed = subprocess.run([str(executable), "call", str(args.endpoint), str(timeout), "1", str(command), *argv],
                                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                           env=environment, timeout=timeout / 1000 + 15)
                if completed.returncode:
                    raise RuntimeError("Native RPC client failed: " + completed.stderr.decode("utf-8", "replace"))
                pieces = completed.stdout.split(b"\n", 5)
                if len(pieces) != 6:
                    raise RuntimeError("Malformed native client result")
                error, code, elapsed, size, diagnostic_size = (int(value) for value in pieces[:5])
                if len(pieces[5]) != size + diagnostic_size:
                    raise RuntimeError("Truncated native client result")
                output = pieces[5][:size].decode("utf-8", "strict")
                diagnostic = pieces[5][size:].decode("utf-8", "strict")
                record = {"label": label, "command": str(command), "argv": argv,
                          "stdinEnabled": False, "brokerControlConnectionHapBytes": 0,
                          "error": error, "exitCode": code, "elapsedMs": elapsed, "output": output, "diagnostic": diagnostic}
                calls.append(record)
                if error or code not in allowed_codes:
                    raise RuntimeError("RPC failed: " + json.dumps(record, ensure_ascii=False))
                if is_hdc and any(marker in output.lower() for marker in ("[fail]", "[error]", "error:", "failed to ", "install failed", "ability failed", "process failed")):
                    raise RuntimeError("HDC reported failure: " + json.dumps(record, ensure_ascii=False))
                return output

            def hdc(label, argv, **kwargs):
                return rpc(label, args.hdc, ["-t", args.target, *argv], is_hdc=True, **kwargs)

            def target_network_node(label, script, values):
                return json.loads(rpc(label + "-verified-self-device-network", args.node,
                                      ["-e", script, *(str(value) for value in values)]).strip())

            def mark(name, **values):
                passed.append({"name": name, "passed": True, **values})
                print("PASS: " + name, flush=True)

            def install():
                pids = hdc("editor-pidof-before-install", ["shell", "pidof", shlex.quote(args.editor_process)], allowed_codes=(0, 1)).strip()
                if pids:
                    report["installationSkipped"] = {"reason": "Editor process is running", "pidofOutput": pids}
                    print("SKIP: replacement install while editor is running", flush=True)
                    return
                mark("editor-absent-before-explicit-replacement-install", process=args.editor_process)
                installed = hdc("explicit-signed-editor-replacement-install", ["install", "-r", str(args.install_hap)], timeout=180000)
                if not any(marker in installed.lower() for marker in ("install success", "install bundle successfully")):
                    raise AssertionError("HDC did not positively confirm package install success")
                report["installedHap"] = str(args.install_hap)
                with args.install_hap.open("rb") as hap_file:
                    report["installedHapSha256"] = hashlib.file_digest(hap_file, "sha256").hexdigest()
                mark("native-broker-HDC-signed-editor-replacement-install", hap=str(args.install_hap))

            try:
                targets = rpc("connected-targets", args.hdc, ["list", "targets"], is_hdc=True)
                if args.target not in targets.splitlines():
                    raise AssertionError("Requested real target is not connected")
                mark("requested-real-target-connected")
                if args.shared_hap:
                    with args.shared_hap.open('rb') as hap_file:
                        local_sha = hashlib.file_digest(hap_file, 'sha256').hexdigest()
                    local_size = args.shared_hap.stat().st_size
                    shared = json.loads(rpc("broker-shared-project-HAP-stat-hash", args.node,
                                            ["-e", SHARED_HAP_SCRIPT, str(args.shared_hap)], timeout=120000).strip())
                    expected_shared = {"hap": str(args.shared_hap), "size": local_size, "sha256": local_sha}
                    if shared != expected_shared:
                        raise AssertionError("Broker shared HAP bytes differ from project file")
                    report["sharedHap"] = shared
                    mark("shared-project-HAP-readable-by-path-without-control-stdin", **shared)
                if args.install_only:
                    report["portsSkipped"] = True
                    install()
                    report["passed"] = True
                    print("OHOS_BROKER_DEVICE_INSTALL_PASS" if "installedHap" in report else "OHOS_BROKER_DEVICE_INSTALL_SKIPPED", flush=True)
                    return
                if args.same_device_broker_node:
                    target_address, _, _ = args.target.rpartition(":")
                    socket.inet_aton(target_address)
                    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as locality_probe:
                        locality_probe.bind((target_address, 0))
                    target_netns = hdc("target-HDC-network-namespace", ["shell", "readlink", "/proc/self/ns/net"]).strip()
                    broker_namespace = json.loads(rpc("broker-Node-network-namespace", args.node, ["-e", NAMESPACE_SCRIPT]).strip())
                    local_netns = os.readlink("/proc/self/ns/net")
                    if not re.fullmatch(r"net:\[\d+\]", target_netns) or broker_namespace != {"netns": target_netns} or local_netns != target_netns:
                        raise AssertionError("Self HDC mode requires a local target address and the exact same network namespace")
                    report["portClientContext"] = {"mode": "broker Node as self-device TCP client; not target-shell execution", "locallyBoundTargetAddress": target_address, **broker_namespace, "echoListenerNetns": local_netns}
                    mark("self-device-address-and-HDC-network-namespace-confirmed", targetAddress=target_address, **broker_namespace)
                baseline_text = hdc("fport-before", ["fport", "ls"])
                baseline = rules(baseline_text)
                report["rulesBefore"] = baseline_text
                used = {int(value) for value in re.findall(r"\btcp:(\d+)\b", baseline_text)}
                nonce = secrets.token_bytes(32)
                echo = Echo(nonce)
                if echo.port in used:
                    raise AssertionError("Fresh host port already occurs in HDC rules")
                remote = target_network_node("reserve-distinct-remote-port", PORT_SCRIPT, [json.dumps(sorted(used | {echo.port}))])
                remote_port = remote["port"]
                if not isinstance(remote_port, int) or not 1 <= remote_port <= 65535 or remote_port == echo.port or remote_port in used:
                    raise AssertionError("Remote port is invalid, colliding, or equal to host port")
                pair = ("tcp:" + str(remote_port), "tcp:" + str(echo.port))
                # Claim ownership only after confirmed successful HDC creation.
                hdc("create-owned-rport", ["rport", *pair])
                owned_pair = pair
                created_text = hdc("fport-after-create", ["fport", "ls"])
                created = rules(created_text)
                added = created - baseline
                if baseline - created or sum(added.values()) != 1 or not any(pair[0] + " " + pair[1] in row for row in added):
                    raise AssertionError("Owned reverse pair was not the sole added HDC rule")
                report["ownedPair"] = {"devicePort": remote_port, "hostPort": echo.port, "listing": created_text}
                echo.start()
                reply = target_network_node("remote-device-echo-roundtrip", ECHO_SCRIPT, [remote_port, nonce.hex()])
                expected = b"godot-broker-echo:" + nonce
                if reply.get("nonce") != nonce.hex() or reply.get("response") != expected.hex() or reply.get("size") != len(expected):
                    raise AssertionError("Device did not receive exact host echo bytes")
                echo.close()
                echo = None
                mark("real-owned-reverse-port-binary-roundtrip", devicePort=remote_port, hostPort=int(pair[1][4:]), nonceHex=nonce.hex(), responseBytes=len(expected))
                hdc("remove-owned-rport", ["fport", "rm", *owned_pair])
                owned_pair = None
                after = hdc("fport-after-cleanup", ["fport", "ls"])
                report["rulesAfter"] = after
                if rules(after) != baseline:
                    raise AssertionError("HDC rules differ after removing only our owned pair")
                mark("owned-rport-cleanup-preserves-all-existing-rules")
                if args.install_hap:
                    install()
                report["passed"] = True
                print("OHOS_BROKER_DEVICE_PASS", flush=True)
            finally:
                # This block runs while the signed native fixture still exists.
                # Only confirmed resources owned by this run may be removed.
                if echo:
                    try:
                        echo.close()
                    except BaseException as error:
                        cleanup_errors.append("echo: " + repr(error))
                if owned_pair:
                    try:
                        hdc("failure-cleanup-owned-rport", ["fport", "rm", *owned_pair])
                        owned_pair = None
                        after = hdc("failure-cleanup-rule-audit", ["fport", "ls"])
                        report["rulesAfterFailureCleanup"] = after
                        if baseline is not None and rules(after) != baseline:
                            cleanup_errors.append("HDC rule baseline changed after owned cleanup")
                    except BaseException as error:
                        cleanup_errors.append("owned rport: " + repr(error))
                if cleanup_errors:
                    report["cleanupErrors"] = cleanup_errors
                    raise RuntimeError("Owned resource cleanup failed: " + "; ".join(cleanup_errors))
    except BaseException as error:
        report["passed"] = False
        report["failure"] = repr(error)
        raise
    finally:
        report["remainingOwnedPair"] = owned_pair
        (args.output / "build.json").write_text(json.dumps(builds, indent=2, ensure_ascii=False) + "\n")
        (args.output / "result.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    main()
