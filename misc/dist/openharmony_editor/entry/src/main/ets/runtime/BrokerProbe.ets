// Godot Engine contributors. SPDX-License-Identifier: MIT
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import plugin from 'libentry.so';
import { describe, record } from './Diagnostics';

interface BrokerEndpoint {
  protocolVersion: number;
  transport: string;
  mode: string;
  host: string;
  port: number;
  instanceId: string;
}

const ENDPOINT_FILE: string = '/storage/Users/currentUser/.oheco/broker/endpoint.json';

// This shared file contains no credentials. Instance IDs are only for detecting
// stale descriptors. A future execution service needs independent authentication.
export async function probeBrokerConnection(): Promise<void> {
  record('--- broker TCP test ---');
  record(`broker_discovery_file=${ENDPOINT_FILE}`);
  let step: string = 'discovery_read';
  try {
    const info = fs.statSync(ENDPOINT_FILE);
    if (!info.isFile() || info.size <= 0 || info.size > 4096) {
      throw new Error('Endpoint must be a regular JSON file of at most 4096 bytes');
    }
    // Bound the actual read too, even if another process replaces the file.
    const text: string = fs.readTextSync(ENDPOINT_FILE, { offset: 0, length: info.size });
    step = 'discovery_parse';
    const endpoint: BrokerEndpoint = JSON.parse(text) as BrokerEndpoint;
    if (!endpoint || endpoint.protocolVersion !== 1 || endpoint.transport !== 'tcp' ||
      endpoint.mode !== 'ping-only' || endpoint.host !== '127.0.0.1' ||
      typeof endpoint.port !== 'number' || !Number.isInteger(endpoint.port) ||
      endpoint.port < 1 || endpoint.port > 65535 || typeof endpoint.instanceId !== 'string' ||
      !/^[a-f0-9]{32}$/.test(endpoint.instanceId)) {
      throw new Error('Unsupported endpoint: expected version 1, ping-only, TCP IPv4 loopback, valid port and instance ID');
    }
    record(`broker_discovery=PASS address=127.0.0.1:${endpoint.port} instance=${endpoint.instanceId}`);
    step = 'binding';
    // Socket operations run on a native worker with a three-second total deadline.
    const report: string = await plugin.probeBrokerTcp(endpoint.port, endpoint.instanceId);
    const lines: string[] = report.split('\n');
    for (const line of lines) {
      if (line.length > 0) { record(line); }
    }
  } catch (error) {
    // Failure to reach the optional ping service must not prevent engine startup.
    record(`broker_probe=FAIL step=${step} message=${describe(error as BusinessError)}`);
  }
}
