#!/usr/bin/env node
// The signalling protocol over a real connection, against a server that is
// already running -- a deployment, say -- or, with no --url, against one it
// starts itself. The same checks run in-process under `npm test` in server/.
//
//     node tools/signal-smoke.mjs                 # start a server on a free port
//     node tools/signal-smoke.mjs --url ws://127.0.0.1:8765
//     node tools/signal-smoke.mjs --url https://signal.example.org/
//     node tools/signal-smoke.mjs --url https://signal.example.org/ --poll
//
// A ws:// or wss:// url is a WebSocket; http:// or https:// is the HTTP
// session -- server push by SSE, frames by POST -- for a host that will not
// upgrade a connection. That one falls back to long polling by itself when a
// proxy holds the stream back; --poll starts there. With no --url, --http
// runs the checks over HTTP against the server it starts.
//
// Exits non-zero on the first failed check. Needs `npm ci` in server/ first,
// for the in-process server's `ws`.

import { parseArgs } from 'node:util';

import { IceConfig } from '../server/src/ice.js';
import { log } from '../server/src/log.js';
import { SignalServer, run } from '../server/src/server.js';
import { smoke } from '../server/test/smoke.js';

const { values: args } = parseArgs({
  options: {
    url: { type: 'string' },
    http: { type: 'boolean', default: false },
    poll: { type: 'boolean', default: false },
  },
});

let checks = 0;
const check = (ok, what) => {
  ++checks;
  console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${what}`);
  if (!ok)
    process.exit(1);
};

let running = null;
let url = args.url;
if (!url) {
  log.quiet();
  const server = new SignalServer(new IceConfig({ stunUrls: ['stun:stun.l.google.com:19302'] }));
  running = await run(server, { host: '127.0.0.1', port: 0 });
  url = `${args.http || args.poll ? 'http' : 'ws'}://127.0.0.1:${running.address().port}`;
}

const how = /^https?:/i.test(url) ? (args.poll ? ', HTTP, long polling' : ', HTTP') : '';
console.log(`signalling smoke against ${url}${how}`);
try {
  await smoke(url, check, { poll: args.poll });
} catch (error) {
  console.log(`  FAIL  ${error.message}`);
  process.exit(1);
} finally {
  await running?.close();
}
console.log(`\n${checks} checks passed`);
