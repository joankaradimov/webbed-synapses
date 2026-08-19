// The protocol over a real socket, against in-process servers on free ports.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { IceConfig } from '../src/ice.js';
import { log } from '../src/log.js';
import { SignalServer, run } from '../src/server.js';
import { Client, smoke } from './smoke.js';

log.quiet();

const ice = new IceConfig({ stunUrls: ['stun:stun.example.org:3478'] });
const serve = options => run(new SignalServer(ice, options), { host: '127.0.0.1', port: 0 });
const urlOf = running => `ws://127.0.0.1:${running.address().port}`;

test('the protocol over a real WebSocket', async t => {
  const running = await serve();
  try {
    await smoke(urlOf(running), (ok, what) => {
      t.diagnostic(`${ok ? 'ok  ' : 'FAIL'}  ${what}`);
      assert.ok(ok, what);
    });
  } finally {
    await running.close();
  }
});

test('a restart: clients are told 1012, and come back as themselves', async () => {
  const before = await serve({ secret: 'shared' });
  const host = await Client.open(urlOf(before), 'host');
  const welcome = await host.hello();
  host.send({ type: 'advertise', nonce: 'n', name: 'game', stat: '', state: 0, max_players: 4 });
  const game = (await host.recv('advertised')).game;

  await before.close({ code: 1012, reason: 'service restart' });
  assert.equal((await host.closed).code, 1012);

  const after = await serve({ secret: 'shared' });
  const back = await Client.open(urlOf(after), 'host');
  try {
    assert.equal((await back.hello('SEXP', 1, { resume: welcome.resume })).peer, welcome.peer,
                 'the same id, from a server that never saw it');
    back.send({ type: 'advertise', nonce: 'n', name: 'game', stat: '', state: 0, max_players: 4 });
    assert.equal((await back.recv('advertised')).game, game, 'and the same game');
  } finally {
    back.close();
    await after.close();
  }
});
