// The protocol over HTTP sessions -- SSE down, POSTs up -- against in-process
// servers on free ports. The same server answers WebSocket upgrades on the
// same port, so the two kinds of client meet in one game.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { IceConfig } from '../src/ice.js';
import { log } from '../src/log.js';
import { SignalServer, run } from '../src/server.js';
import { Client, smoke } from './smoke.js';

log.quiet();

const ice = new IceConfig({ stunUrls: ['stun:stun.example.org:3478'] });
const serve = (options = {}, http = {}) =>
  run(new SignalServer(ice, options), { host: '127.0.0.1', port: 0, ...http });
const httpOf = running => `http://127.0.0.1:${running.address().port}`;
const wsOf = running => `ws://127.0.0.1:${running.address().port}`;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));

/** Reads SSE events from a response until `count` have arrived or it ends. */
async function events(response, count = Infinity) {
  const out = [];
  const decoder = new TextDecoder();
  let buffer = '';
  let event = {};
  for await (const chunk of response.body) {
    buffer += decoder.decode(chunk, { stream: true });
    let newline;
    while ((newline = buffer.indexOf('\n')) >= 0) {
      const line = buffer.slice(0, newline);
      buffer = buffer.slice(newline + 1);
      if (line === '') {
        if (event.data !== undefined)
          out.push(event);
        event = {};
        if (out.length >= count)
          return out;
      } else if (!line.startsWith(':')) {
        const [field, ...rest] = line.split(':');
        event[field] = rest.join(':').replace(/^ /, '');
      }
    }
  }
  return out;
}

test('the protocol over HTTP, streaming', async t => {
  const running = await serve();
  try {
    await smoke(httpOf(running), (ok, what) => {
      t.diagnostic(`${ok ? 'ok  ' : 'FAIL'}  ${what}`);
      assert.ok(ok, what);
    });
  } finally {
    await running.close();
  }
});

test('the protocol over HTTP, long polling', async t => {
  const running = await serve();
  try {
    await smoke(httpOf(running), (ok, what) => {
      t.diagnostic(`${ok ? 'ok  ' : 'FAIL'}  ${what}`);
      assert.ok(ok, what);
    }, { poll: true });
  } finally {
    await running.close();
  }
});

test('a WebSocket host and an HTTP joiner meet in one game', async () => {
  const running = await serve();
  const host = await Client.open(wsOf(running), 'host');
  const joiner = await Client.open(httpOf(running), 'joiner');
  try {
    await host.hello();
    await joiner.hello();
    host.send({ type: 'advertise', name: 'mixed', stat: '', state: 0, max_players: 4 });
    const game = (await host.recv('advertised')).game;
    joiner.send({ type: 'join', game });
    assert.equal((await joiner.recv('joined')).host, host.id);
    assert.equal((await host.recv('peer_joined')).peer, joiner.id);

    joiner.send({ type: 'signal', to: host.id, data: { kind: 'request' } });
    assert.deepEqual((await host.recv('signal')).data, { kind: 'request' });
    host.send({ type: 'signal', to: joiner.id, data: { kind: 'offer', sdp: 'v=0\r\n' } });
    const offer = await joiner.recv('signal');
    assert.equal(offer.from, host.id);
    assert.equal(offer.data.sdp, 'v=0\r\n', 'carried byte for byte, CR and all');
  } finally {
    host.close();
    joiner.close();
    await running.close();
  }
});

test('a cut stream loses nothing: events wait until acknowledged', async () => {
  const running = await serve();
  const base = httpOf(running);
  try {
    const { session } = await (await fetch(`${base}/session`, { method: 'POST' })).json();
    const post = text => fetch(`${base}/session/${session}`, { method: 'POST', body: text });
    await post(JSON.stringify({ type: 'hello', product: 'SEXP', version: 1, name: 'cut' }));

    const first = new AbortController();
    const response = await fetch(`${base}/session/${session}/events?after=0`,
                                 { signal: first.signal });
    const [ready, welcome] = await events(response, 2);
    assert.equal(ready.event, 'ready');
    assert.equal(welcome.id, '1');
    assert.equal(JSON.parse(welcome.data).type, 'welcome');
    first.abort(); // and the stream is gone

    // Answered while nobody is listening.
    await post([JSON.stringify({ type: 'list' }), JSON.stringify({ type: 'list' })].join('\n'));

    // Not acknowledged, so the welcome comes again, then what followed.
    const again = await events(await fetch(`${base}/session/${session}/events?after=0&poll=1`));
    assert.deepEqual(again.map(event => event.id), ['1', '2', '3']);
    // Acknowledged now: only what follows.
    const rest = await events(await fetch(`${base}/session/${session}/events?after=2&poll=1`));
    assert.deepEqual(rest.map(event => event.id), ['3']);
    assert.equal(JSON.parse(rest[0].data).type, 'games');
  } finally {
    await running.close();
  }
});

test('a client that stops asking keeps its place for the grace period, then goes', async () => {
  const running = await serve({ grace: 0.5 }, { idleSeconds: 0.2 });
  const base = httpOf(running);
  const member = await Client.open(wsOf(running), 'member');
  try {
    const { session } = await (await fetch(`${base}/session`, { method: 'POST' })).json();
    await fetch(`${base}/session/${session}`, {
      method: 'POST',
      body: [
        JSON.stringify({ type: 'hello', product: 'SEXP', version: 1, name: 'vanishing' }),
        JSON.stringify({ type: 'advertise', name: 'brief', stat: '', state: 0, max_players: 4 }),
      ].join('\n'),
    });
    const [, , advertised] = await events(
      await fetch(`${base}/session/${session}/events?after=0`), 3);
    const game = JSON.parse(advertised.data).game;

    await member.hello();
    member.send({ type: 'join', game });
    await member.recv('joined');

    // The host read its stream that far and stopped; it never asks again.
    const started = Date.now();
    const closed = await member.recv('game_closed', 5000);
    assert.equal(closed.game, game);
    assert.ok(Date.now() - started >= 400, 'not before the grace period ran out');
  } finally {
    member.close();
    await running.close();
  }
});

test('a restart: HTTP sessions are told 1012, and their peers come back as themselves', async () => {
  const before = await serve({ secret: 'shared' });
  const host = await Client.open(httpOf(before), 'host');
  const welcome = await host.hello();
  host.send({ type: 'advertise', nonce: 'n', name: 'game', stat: '', state: 0, max_players: 4 });
  const game = (await host.recv('advertised')).game;

  await before.close({ code: 1012, reason: 'service restart' });
  const closed = await host.closed;
  assert.equal(closed.code, 1012);
  assert.equal(closed.reason, 'service restart');

  const after = await serve({ secret: 'shared' });
  const back = await Client.open(httpOf(after), 'host');
  try {
    assert.equal((await back.hello('SEXP', 1, { resume: welcome.resume })).peer, welcome.peer);
    back.send({ type: 'advertise', nonce: 'n', name: 'game', stat: '', state: 0, max_players: 4 });
    assert.equal((await back.recv('advertised')).game, game);
  } finally {
    back.close();
    await after.close();
  }
});

test('an HTTP session whose peer resumed elsewhere is told, and closed with 4000', async () => {
  const running = await serve({ secret: 'shared' });
  const first = await Client.open(httpOf(running), 'first');
  const second = await Client.open(wsOf(running), 'second');
  try {
    const welcome = await first.hello();
    await second.hello('SEXP', 1, { resume: welcome.resume });
    assert.equal((await first.recv('error')).code, 'replaced');
    assert.equal((await first.closed).code, 4000);
  } finally {
    second.close();
    await running.close();
  }
});

test('HTTP: the plumbing around sessions', async () => {
  const running = await serve();
  const base = httpOf(running);
  try {
    const root = await fetch(`${base}/`);
    assert.equal(root.status, 200, 'a plain GET says what this is');
    assert.match(await root.text(), /signalling server/);

    const preflight = await fetch(`${base}/session`, { method: 'OPTIONS' });
    assert.equal(preflight.status, 204);
    assert.equal(preflight.headers.get('access-control-allow-origin'), '*');

    const missing = await fetch(`${base}/session/${'0'.repeat(32)}/events`);
    assert.equal(missing.status, 404);

    const created = await fetch(`${base}/prefix/under/a/path/session`, { method: 'POST' });
    assert.equal(created.status, 201, 'mounted under any path');
    assert.equal(created.headers.get('access-control-allow-origin'), '*');
    const { session, stream } = await created.json();
    assert.match(session, /^[0-9a-f]{32}$/);
    assert.equal(stream, 25);

    const badCursor = await fetch(`${base}/session/${session}/events?after=99`);
    assert.equal(badCursor.status, 400, 'no acknowledging what was never sent');

    const tooLarge = await fetch(`${base}/session/${session}`,
                                 { method: 'POST', body: 'x'.repeat(1024 * 1024) });
    assert.equal(tooLarge.status, 413);

    const controller = new AbortController();
    const live = await fetch(`${base}/session/${session}/events`, { signal: controller.signal });
    assert.equal(live.headers.get('content-type'), 'text/event-stream; charset=utf-8');
    assert.equal(live.headers.get('x-accel-buffering'), 'no');
    controller.abort();

    assert.equal((await fetch(`${base}/session/${session}/close`, { method: 'POST' })).status, 204);
    assert.equal((await fetch(`${base}/session/${session}`, { method: 'POST', body: '' })).status,
                 404, 'closed is closed');
  } finally {
    await running.close();
  }
});
