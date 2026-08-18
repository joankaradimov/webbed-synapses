#!/usr/bin/env node
// Entry point. Configuration is environment first, flags second, so the same
// image runs in a container and on a developer's machine unchanged.
//
//     node src/main.js [--host 0.0.0.0] [--port 8765] [--stun url,url]
//                      [--turn url,url --turn-secret secret] [--turn-ttl 3600]
//                      [--room-ttl 30] [--verbose]

import { parseArgs } from 'node:util';

import { IceConfig } from './ice.js';
import { log } from './log.js';
import { SignalServer, run } from './server.js';

const env = process.env;
const urls = value => (value ?? '').split(',').map(url => url.trim()).filter(Boolean);

function fail(message) {
  process.stderr.write(`snps-signal: ${message}\n`);
  process.exit(2);
}

let args;
try {
  ({ values: args } = parseArgs({
    options: {
      host: { type: 'string', default: env.SNPS_HOST ?? '0.0.0.0' },
      port: { type: 'string', default: env.SNPS_PORT ?? '8765' },
      stun: { type: 'string', default: env.SNPS_STUN ?? '' },
      turn: { type: 'string', default: env.SNPS_TURN ?? '' },
      'turn-secret': { type: 'string', default: env.SNPS_TURN_SECRET },
      'turn-ttl': { type: 'string', default: env.SNPS_TURN_TTL ?? '3600' },
      'room-ttl': { type: 'string', default: env.SNPS_ROOM_TTL ?? '30' },
      verbose: { type: 'boolean', short: 'v', default: false },
      help: { type: 'boolean', short: 'h', default: false },
    },
  }));
} catch (error) {
  fail(error.message);
}

if (args.help) {
  process.stdout.write(`usage: snps-signal [--host H] [--port P] [--stun URLS] [--turn URLS]
                   [--turn-secret S] [--turn-ttl SECONDS] [--room-ttl SECONDS] [-v]

Signalling and game directory for snps. Every option can come from the
environment instead: SNPS_HOST, SNPS_PORT, SNPS_STUN, SNPS_TURN,
SNPS_TURN_SECRET, SNPS_TURN_TTL, SNPS_ROOM_TTL.
`);
  process.exit(0);
}

const number = (name, text, { integer = false } = {}) => {
  const value = Number(text);
  if (text === '' || !Number.isFinite(value) || (integer && !Number.isInteger(value)) || value < 0)
    fail(`--${name} must be a ${integer ? 'whole ' : ''}number, not ${JSON.stringify(text)}`);
  return value;
};

const port = number('port', args.port, { integer: true });
const turnTtl = number('turn-ttl', args['turn-ttl'], { integer: true });
const roomTtl = number('room-ttl', args['room-ttl']);
const ice = new IceConfig({
  stunUrls: urls(args.stun),
  turnUrls: urls(args.turn),
  turnSecret: args['turn-secret'] || null,
  turnTtl,
});
if (ice.turnUrls.length && !ice.turnSecret)
  fail('--turn-secret is required when --turn is set');

log.verbose(args.verbose);
const server = new SignalServer(ice, { roomTtl });
const running = await run(server, { host: args.host, port, roomTtl });

for (const signal of ['SIGINT', 'SIGTERM'])
  process.on(signal, () => running.close().then(() => process.exit(0)));
