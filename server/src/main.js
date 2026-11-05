#!/usr/bin/env node
// Entry point. Configuration is environment first, flags second, so the same
// image runs in a container and on a developer's machine unchanged.
//
//     node src/main.js [--host 0.0.0.0] [--port 8765] [--stun url,url]
//                      [--turn url,url --turn-secret secret] [--turn-ttl 3600]
//                      [--secret secret] [--grace 20] [--room-ttl 30]
//                      [--http-stream 25] [--verbose]
//
// One port serves both WebSocket upgrades and HTTP sessions, for clients
// behind a host whose proxy will not upgrade (src/http.js).

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
      // PORT is what a hosting platform hands its app; SNPS_PORT still wins.
      port: { type: 'string', default: env.SNPS_PORT ?? env.PORT ?? '8765' },
      stun: { type: 'string', default: env.SNPS_STUN ?? '' },
      turn: { type: 'string', default: env.SNPS_TURN ?? '' },
      'turn-secret': { type: 'string', default: env.SNPS_TURN_SECRET },
      'turn-ttl': { type: 'string', default: env.SNPS_TURN_TTL ?? '3600' },
      secret: { type: 'string', default: env.SNPS_SECRET },
      grace: { type: 'string', default: env.SNPS_GRACE ?? '20' },
      'room-ttl': { type: 'string', default: env.SNPS_ROOM_TTL ?? '30' },
      'http-stream': { type: 'string', default: env.SNPS_HTTP_STREAM ?? '25' },
      // For testing clients against the hosts HTTP sessions are for.
      'no-websocket': { type: 'boolean', default: false },
      'http-buffered': { type: 'boolean', default: false },
      verbose: { type: 'boolean', short: 'v', default: false },
      help: { type: 'boolean', short: 'h', default: false },
    },
  }));
} catch (error) {
  fail(error.message);
}

if (args.help) {
  process.stdout.write(`usage: snps-signal [--host H] [--port P] [--stun URLS] [--turn URLS]
                   [--turn-secret S] [--turn-ttl SECONDS] [--secret S]
                   [--grace SECONDS] [--room-ttl SECONDS]
                   [--http-stream SECONDS] [-v]

Signalling and game directory for snps.

--secret signs the resume tokens that give a reconnecting client its peer id
back. Every instance that should honour another's tokens -- a restarted one,
say -- needs the same one. Without it, tokens last as long as the process.
--grace is how long a peer whose connection dropped keeps its place; 0 drops
it at once.

The same port answers WebSocket upgrades and plain HTTP, for clients behind a
host that will not upgrade a connection: POST /session, then SSE from
GET /session/ID/events. --http-stream is how long one of those streams, or a
long poll, stays open before the client asks again. Keep it below the shortest
request timeout of any proxy in front of this server.

For testing clients: --no-websocket answers an upgrade like a proxy that does
not do WebSockets, and --http-buffered holds every response back until it
ends, like a proxy that buffers.

Every option can come from the environment instead: SNPS_HOST, SNPS_PORT,
SNPS_STUN, SNPS_TURN, SNPS_TURN_SECRET, SNPS_TURN_TTL, SNPS_SECRET,
SNPS_GRACE, SNPS_ROOM_TTL, SNPS_HTTP_STREAM.

SIGTERM closes every connection with 1012, "service restart"; SIGINT with
1001, "going away". Clients reconnect either way.
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
const grace = number('grace', args.grace);
const roomTtl = number('room-ttl', args['room-ttl']);
const streamSeconds = number('http-stream', args['http-stream']);
if (streamSeconds < 1)
  fail('--http-stream must be at least 1 second');
const ice = new IceConfig({
  stunUrls: urls(args.stun),
  turnUrls: urls(args.turn),
  turnSecret: args['turn-secret'] || null,
  turnTtl,
});
if (ice.turnUrls.length && !ice.turnSecret)
  fail('--turn-secret is required when --turn is set');

log.verbose(args.verbose);
const secret = args.secret || null;
if (!secret)
  log.warning('no --secret: peer ids survive a dropped connection, but not a restart of this server');
const server = new SignalServer(ice, { roomTtl, grace, secret });
const running = await run(server, {
  host: args.host, port, roomTtl, streamSeconds,
  websocket: !args['no-websocket'], buffered: args['http-buffered'],
});

const endings = { SIGINT: [1001, 'going away'], SIGTERM: [1012, 'service restart'] };
for (const [signal, [code, reason]] of Object.entries(endings)) {
  process.on(signal, () => {
    log.info(`${signal}: closing every connection with ${code}, "${reason}"`);
    running.close({ code, reason }).then(() => process.exit(0));
  });
}
