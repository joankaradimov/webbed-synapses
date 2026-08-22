#!/usr/bin/env node
// The signalling protocol over a real WebSocket, against a server that is
// already running -- a deployment, say -- or, with no --url, against one it
// starts itself. The same checks run in-process under `npm test` in server/.
//
//     node tools/signal-smoke.mjs                 # start a server on a free port
//     node tools/signal-smoke.mjs --url ws://127.0.0.1:8765
//
// Exits non-zero on the first failed check. Needs `npm ci` in server/ first,
// for the in-process server's `ws`.

import { parseArgs } from 'node:util';

import { IceConfig } from '../server/src/ice.js';
import { log } from '../server/src/log.js';
import { SignalServer, run } from '../server/src/server.js';
import { smoke } from '../server/test/smoke.js';

const { values: args } = parseArgs({ options: { url: { type: 'string' } } });

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
  url = `ws://127.0.0.1:${running.address().port}`;
}

console.log(`signalling smoke against ${url}`);
try {
  await smoke(url, check);
} catch (error) {
  console.log(`  FAIL  ${error.message}`);
  process.exit(1);
} finally {
  await running?.close();
}
console.log(`\n${checks} checks passed`);
