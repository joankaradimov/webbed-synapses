// smoke.js against an in-process server on a free port, over a real socket.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { IceConfig } from '../src/ice.js';
import { log } from '../src/log.js';
import { SignalServer, run } from '../src/server.js';
import { smoke } from './smoke.js';

log.quiet();

test('the protocol over a real WebSocket', async t => {
  const server = new SignalServer(new IceConfig({ stunUrls: ['stun:stun.example.org:3478'] }));
  const running = await run(server, { host: '127.0.0.1', port: 0 });
  try {
    await smoke(`ws://127.0.0.1:${running.address().port}`, (ok, what) => {
      t.diagnostic(`${ok ? 'ok  ' : 'FAIL'}  ${what}`);
      assert.ok(ok, what);
    });
  } finally {
    await running.close();
  }
});
