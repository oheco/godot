'use strict';

// Diagnostic only: one managed v1 request, no shell, reconnect, or service changes.
const fs = require('node:fs');
const net = require('node:net');
const { TextDecoder } = require('node:util');

const ENDPOINT_FILE = '/storage/Users/currentUser/.oheco/broker/endpoint';
const HDC = '/storage/Users/currentUser/.oheco/packages/ohos-sdk-toolchains/26.0.0.35-Beta/hdc';
const ARGV = ['list', 'targets'];
const GREETING = Buffer.from('OHECOB1\n', 'ascii');
const MAX_FRAME = 1048576;
const MAX_OUTPUT = 1048576;
const ERROR_NAMES = ['OK', 'UNAVAILABLE', 'PROTOCOL', 'SPAWN_FAILED', 'CONNECTION_LOST', 'INVALID_ARGUMENT', 'TIMEOUT', 'IO', 'LIMIT'];

function diagnostic(code, stage, message, native = null) {
  return { code, name: ERROR_NAMES[code], stage, message, native };
}

function endpoint() {
  let fd;
  try {
    fd = fs.openSync(ENDPOINT_FILE, 'r');
    const bytes = Buffer.alloc(65);
    let size = 0;
    while (size < bytes.length) {
      const count = fs.readSync(fd, bytes, size, bytes.length - size, null);
      if (count === 0) break;
      size += count;
    }
    if (size === 0 || size > 64) throw new Error('discovery file must contain 1..64 bytes');
    const text = bytes.subarray(0, size).toString('ascii');
    // Buffer's ASCII decoder masks high bits, so require an exact byte round-trip.
    const match = /^127\.0\.0\.1:([0-9]+)(?:\r?\n)?$/.exec(text);
    if (!bytes.subarray(0, size).equals(Buffer.from(text, 'ascii')) || !match || match[0].length !== text.length) {
      throw new Error('invalid discovery endpoint');
    }
    const port = Number(match[1]);
    if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('invalid discovery port');
    return { host: '127.0.0.1', port };
  } finally {
    if (fd !== undefined) fs.closeSync(fd);
  }
}

function uint32(value) {
  const bytes = Buffer.alloc(4);
  bytes.writeUInt32BE(value);
  return bytes;
}

function string(value) {
  const bytes = Buffer.from(value, 'utf8');
  if (value.includes('\0') || bytes.length > MAX_FRAME) throw new Error('invalid request string');
  return Buffer.concat([uint32(bytes.length), bytes]);
}

function startFrame() {
  const payload = Buffer.concat([
    string(HDC), string(''), uint32(ARGV.length), ...ARGV.map(string),
    uint32(0), Buffer.from([0]), // no environment overrides, stdin disabled
  ]);
  if (ARGV.length > 4096 || payload.length > MAX_FRAME) throw new Error('request exceeds v1 limits');
  return Buffer.concat([Buffer.from([1]), uint32(payload.length), payload]);
}

function serverError(payload) {
  if (payload.length < 8) throw new Error('short ERROR payload');
  const code = payload.readUInt32BE(0);
  const length = payload.readUInt32BE(4);
  if (code < 1 || code > 8 || length !== payload.length - 8) throw new Error('invalid ERROR payload');
  // Wire strings require strict UTF-8 and no embedded NUL; output streams do not.
  const message = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(payload.subarray(8));
  if (message.includes('\0')) throw new Error('NUL in ERROR message');
  return diagnostic(code, 'server', message);
}

function exitResult(payload) {
  if (payload.length !== 12) throw new Error('invalid EXIT payload length');
  const reason = payload.readUInt32BE(0);
  const code = payload.readInt32BE(4);
  const signal = payload.readUInt32BE(8);
  const normal = code >= 0 && code <= 255 && signal === 0;
  const signalled = code === -1 && signal > 0;
  if ((reason === 0 && !normal) || (reason === 1 && !signalled) ||
      (reason === 2 && !normal && !signalled) || reason > 2) throw new Error('invalid EXIT values');
  return { reason, reason_name: ['normal', 'signal', 'cancelled'][reason], code, signal };
}

async function runProbe() {
  const result = {
    protocol: 'v1', endpoint_file: ENDPOINT_FILE, endpoint: null,
    executable: HDC, argv: ARGV.slice(), cwd: '', stdin_enabled: false,
    uid: typeof process.getuid === 'function' ? process.getuid() : null,
    started: false, stdout: '', stderr: '', stdout_base64: '', stderr_base64: '',
    exit: null, error: null,
  };
  const began = Date.now();
  let address;
  try {
    if (process.argv.length > 2) throw new Error('this diagnostic accepts no command arguments');
    address = endpoint();
    result.endpoint = `${address.host}:${address.port}`;
  } catch (error) {
    const code = process.argv.length > 2 ? 5 : 1;
    result.error = diagnostic(code, code === 5 ? 'arguments' : 'discovery', error.message, error.code || null);
    result.elapsed_ms = Date.now() - began;
    return result;
  }

  return new Promise((resolve) => {
    let socket;
    let timer;
    let finished = false;
    let stage = 'connect';
    let sentStart = false;
    let terminal = null;
    let buffered = Buffer.alloc(0);
    let outputSize = 0;
    const stdout = [];
    const stderr = [];

    function finish(error = null) {
      if (finished) return;
      finished = true;
      clearTimeout(timer);
      if (socket) socket.destroy(); // release only this connection; no explicit CANCEL/retry
      const out = Buffer.concat(stdout);
      const err = Buffer.concat(stderr);
      result.stdout = out.toString('utf8');
      result.stderr = err.toString('utf8');
      // Preserve arbitrary binary stream bytes as well as convenient text.
      result.stdout_base64 = out.toString('base64');
      result.stderr_base64 = err.toString('base64');
      result.error = error || (terminal && terminal.error) || null;
      if (!error && terminal && terminal.exit) result.exit = terminal.exit;
      result.elapsed_ms = Date.now() - began;
      resolve(result);
    }

    function deadline(nextStage, milliseconds) {
      stage = nextStage;
      clearTimeout(timer);
      timer = setTimeout(() => {
        const code = stage === 'connect' ? 1 : (stage === 'handshake' ? 2 : 6);
        finish(diagnostic(code, stage, `${stage} deadline exceeded; no retry`));
      }, milliseconds);
    }

    function parseFrame(type, payload) {
      if (terminal) throw new Error('message after terminal response');
      if (type === 9) {
        terminal = { error: serverError(payload) };
      } else if (type === 2) {
        if (result.started || payload.length !== 0) throw new Error('invalid or repeated STARTED');
        result.started = true;
        deadline('command', 15000);
      } else if (type === 5 || type === 6) {
        if (!result.started || payload.length < 1 || payload.length > 65536) throw new Error('invalid or out-of-order stream frame');
        outputSize += payload.length;
        if (outputSize > MAX_OUTPUT) {
          finish(diagnostic(8, stage, 'combined output exceeds diagnostic 1 MiB limit'));
          return;
        }
        (type === 5 ? stdout : stderr).push(Buffer.from(payload));
      } else if (type === 8) {
        if (!result.started) throw new Error('EXIT before STARTED');
        terminal = { exit: exitResult(payload) };
      } else {
        throw new Error(`unexpected v1 server frame type ${type}`);
      }
    }

    function receive(chunk) {
      if (finished) return;
      try {
        buffered = Buffer.concat([buffered, chunk]);
        if (stage === 'handshake') {
          const prefix = buffered.subarray(0, Math.min(buffered.length, GREETING.length));
          if (!prefix.equals(GREETING.subarray(0, prefix.length))) throw new Error('invalid v1 handshake');
          if (buffered.length < GREETING.length) return;
          buffered = buffered.subarray(GREETING.length);
          if (buffered.length !== 0) throw new Error('server frame before START');
          deadline('start', 3000);
          socket.write(startFrame());
          sentStart = true;
        }
        while (!finished && buffered.length >= 5) {
          const type = buffered[0];
          const length = buffered.readUInt32BE(1);
          if (![2, 5, 6, 8, 9].includes(type)) throw new Error(`unexpected v1 server frame type ${type}`);
          if (length > MAX_FRAME) throw new Error('frame exceeds v1 1 MiB limit');
          if ((type === 2 && length !== 0) || (type === 8 && length !== 12) ||
              ((type === 5 || type === 6) && (length < 1 || length > 65536)) ||
              (type === 9 && length < 8)) throw new Error('invalid v1 frame payload length');
          if ((type === 2 && result.started) || ([5, 6, 8].includes(type) && !result.started)) {
            throw new Error('out-of-order v1 frame');
          }
          if (buffered.length < 5 + length) break;
          const payload = buffered.subarray(5, 5 + length);
          buffered = buffered.subarray(5 + length);
          parseFrame(type, payload);
        }
        if (!finished && terminal) {
          if (buffered.length !== 0) throw new Error('trailing bytes after terminal response');
          finish();
        }
      } catch (error) {
        finish(diagnostic(2, stage, error.message));
      }
    }

    try {
      deadline('connect', 3000);
      socket = net.createConnection(address);
      socket.on('connect', () => {
        deadline('handshake', 3000);
        socket.write(GREETING);
      });
      socket.on('data', receive);
      socket.on('error', (error) => {
        const code = stage === 'connect' ? 1 : (stage === 'handshake' ? 2 : (sentStart ? 4 : 7));
        finish(diagnostic(code, stage, error.message, error.code || null));
      });
      socket.on('end', () => {
        if (!finished) finish(diagnostic(stage === 'handshake' ? 2 : (sentStart ? 4 : 1), stage, 'connection ended before terminal response'));
      });
      socket.on('close', () => {
        if (!finished) finish(diagnostic(stage === 'handshake' ? 2 : (sentStart ? 4 : 1), stage, 'connection closed before terminal response'));
      });
    } catch (error) {
      finish(diagnostic(stage === 'connect' ? 1 : 7, stage, error.message, error.code || null));
    }
  });
}

module.exports = { runProbe };
if (require.main === module) {
  runProbe().then((result) => {
    process.stdout.write(`${JSON.stringify(result)}\n`);
    // A nonzero HDC exit remains a normal broker result in JSON.
    process.exitCode = result.error ? 1 : 0;
  }).catch((error) => {
    process.stdout.write(`${JSON.stringify({ error: diagnostic(7, 'client', error.message, error.code || null) })}\n`);
    process.exitCode = 1;
  });
}
