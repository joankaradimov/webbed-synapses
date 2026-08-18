// The whole path through SignalServer, driven by a stand-in connection.
//
// This exercises dispatch, validation, routing and disconnect handling without
// a network. It is worth having as more than a unit test: the first version of
// it found a real bug, where a room was removed from the registry before its
// members were told the game had closed, so nobody was ever notified.

import assert from 'node:assert/strict';
import { beforeEach, test } from 'node:test';

import { IceConfig } from '../src/ice.js';
import { log } from '../src/log.js';
import { SignalServer } from '../src/server.js';

log.quiet(); // refusals are the point of several tests; their warnings are noise

/** Stands in for a ws connection: records what the server sends. */
class FakeSocket {
  constructor(server) {
    this.sent = [];
    this.closedByServer = false;
    this.connection = server.open(this);
  }

  send(text) { this.sent.push(JSON.parse(text)); }
  close() { this.closedByServer = true; }

  say(message) { this.connection.message(JSON.stringify(message)); }
  hangUp() { this.connection.closed(); }

  last(type) {
    for (let i = this.sent.length - 1; i >= 0; --i)
      if (this.sent[i].type === type)
        return this.sent[i];
    return null;
  }

  get id() { return this.last('welcome').peer; }
}

let server;
beforeEach(() => {
  server = new SignalServer(new IceConfig({
    stunUrls: ['stun:stun.example.org:3478'],
    turnUrls: ['turn:relay.example.org:3478'],
    turnSecret: 's3cret',
  }));
});

function connect({ product = 'SEXP', version = 1, name = 'player', ...extra } = {}) {
  const socket = new FakeSocket(server);
  socket.say({ type: 'hello', product, version, name, ...extra });
  return socket;
}

test('hello mints an identity and ICE config', () => {
  const a = connect({ name: 'Joan' });
  const welcome = a.last('welcome');
  assert.equal(Buffer.from(welcome.peer, 'hex').length, 16, 'must fit SNETADDR');
  const [stun, turn] = welcome.ice;
  assert.deepEqual(stun.urls, ['stun:stun.example.org:3478']);
  assert.ok(turn.username.endsWith(welcome.peer), 'TURN credentials are per peer');
  assert.ok(turn.credential, 'and carry an HMAC coturn can verify');
});

test('the directory is scoped by product', () => {
  const host = connect({ product: 'SEXP' });
  host.say({ type: 'advertise', name: 'a game', stat: '', state: 0, max_players: 4 });

  const peer = connect({ product: 'SEXP' });
  peer.say({ type: 'list' });
  assert.equal(peer.last('games').games.length, 1);

  const diablo = connect({ product: 'DRTL' });
  diablo.say({ type: 'list' });
  assert.deepEqual(diablo.last('games').games, [], 'Diablo must not see StarCraft games');
});

test("storm's strings arrive byte for byte", () => {
  // StarCraft separates its stat string's fields with '\r'. Stripping them
  // once made every joiner hide the game, so they must survive untouched.
  const stat = ',44,,6,,f,,4,5ab5438b,10,,Chaos\rUntitled Scenario\r';
  const host = connect();
  host.say({ type: 'advertise', name: 'Caf\xe9\x00 game' + 'x'.repeat(400), stat, state: 0, max_players: 4 });
  const peer = connect();
  peer.say({ type: 'list' });
  const game = peer.last('games').games[0];
  assert.equal(game.stat, stat);
  assert.ok(game.name.startsWith('Caf\xe9 game'), 'only NUL is removed');
  assert.equal(game.name.length, 127, 'storm holds char szGameName[128]');
});

test('join, and relay a negotiation', () => {
  const host = connect({ name: 'host' });
  const joiner = connect({ name: 'joiner' });

  host.say({ type: 'advertise', name: 'game', stat: '', state: 0, max_players: 4 });
  const game = host.last('advertised').game;

  joiner.say({ type: 'join', game });
  assert.equal(joiner.last('joined').host, host.id);
  assert.equal(host.last('peer_joined').peer, joiner.id);

  joiner.say({ type: 'signal', to: host.id, data: { sdp: 'v=0...' } });
  const relayed = host.last('signal');
  assert.equal(relayed.from, joiner.id);
  assert.equal(relayed.data.sdp, 'v=0...', 'the payload is relayed verbatim');

  host.say({ type: 'signal', to: joiner.id, data: { candidate: 'a=candidate:1' } });
  assert.equal(joiner.last('signal').from, host.id);
});

test('broadcast stays inside the room', () => {
  const host = connect();
  const joiner = connect();
  host.say({ type: 'advertise', name: 'game', stat: '', state: 0, max_players: 4 });
  joiner.say({ type: 'join', game: host.last('advertised').game });

  const outsider = connect();
  host.say({ type: 'broadcast', data: { probe: 1 } });

  assert.equal(joiner.last('broadcast').data.probe, 1);
  assert.equal(outsider.last('broadcast'), null, 'must not escape the room');
});

test('a multicast reaches open peers in scope, and no one else', () => {
  // DevilutionX's game-list query: every open player in scope hears it,
  // whether or not it is in a game, and no one else does.
  const asker = connect({ product: 'DVLX', open: true });
  const master = connect({ product: 'DVLX', open: true });
  const closed = connect({ product: 'DVLX' });
  const elsewhere = connect({ product: 'DRTL', open: true });

  asker.say({ type: 'multicast', data: 'cXVlcnk=' });
  assert.deepEqual(master.last('multicast'), { type: 'multicast', from: asker.id, data: 'cXVlcnk=' });
  assert.equal(closed.last('multicast'), null, 'a peer that is not open is not reached');
  assert.equal(elsewhere.last('multicast'), null, 'nor is another product');
  assert.equal(asker.last('multicast'), null, 'nor the sender');
});

test('relay carries the answer back', () => {
  const asker = connect({ product: 'DVLX', open: true });
  const master = connect({ product: 'DVLX', open: true });

  master.say({ type: 'relay', to: asker.id, data: 'YW5zd2Vy' });
  assert.deepEqual(asker.last('relay'), { type: 'relay', from: master.id, data: 'YW5zd2Vy' });

  const closed = connect({ product: 'DVLX' });
  master.say({ type: 'relay', to: closed.id, data: 'YQ==' });
  assert.equal(master.last('error').code, 'not_reachable');
  assert.equal(closed.last('relay'), null);
});

test('open peers negotiate without a room', () => {
  const a = connect({ product: 'DVLX', open: true });
  const b = connect({ product: 'DVLX', open: true });
  a.say({ type: 'signal', to: b.id, data: { kind: 'request' } });
  b.say({ type: 'signal', to: a.id, data: { kind: 'offer', sdp: 'v=0' } });
  assert.equal(b.last('signal').from, a.id);
  assert.equal(a.last('signal').data.kind, 'offer');
  assert.equal(a.last('error'), null);
  assert.equal(b.last('error'), null);
});

test('a multicast has a budget of its own', () => {
  const asker = connect({ product: 'DVLX', open: true });
  connect({ product: 'DVLX', open: true });
  for (let i = 0; i < 4; ++i)
    asker.say({ type: 'multicast', data: 'cQ==' });
  assert.equal(asker.last('error'), null, 'a short burst is fine');
  asker.say({ type: 'multicast', data: 'cQ==' });
  assert.equal(asker.last('error').code, 'rate_limited');
});

test("hello's open must be a boolean", () => {
  const peer = connect({ product: 'DVLX', open: 'yes' });
  assert.equal(peer.last('error').code, 'bad_field');
  assert.equal(peer.last('welcome'), null);
});

test('bad messages are refused, not fatal', () => {
  const host = connect({ product: 'SEXP' });
  host.say({ type: 'advertise', name: 'game', stat: '', state: 0, max_players: 4 });

  const outsider = connect({ product: 'DRTL' });
  outsider.say({ type: 'signal', to: host.id, data: {} });
  assert.equal(outsider.last('error').code, 'wrong_product');

  outsider.say({ type: 'nonsense' });
  assert.equal(outsider.last('error').code, 'unknown_type');

  // Names an Object method rather than a handler; must not reach it.
  outsider.say({ type: 'toString' });
  assert.equal(outsider.last('error').code, 'unknown_type');

  outsider.connection.message('{not json');
  assert.equal(outsider.last('error').code, 'bad_message');

  outsider.say({ type: 'list' });
  assert.notEqual(outsider.last('games'), null, 'the connection survives its mistakes');
});

test('a frame over the limit is refused and the connection closed', () => {
  const loud = connect();
  loud.say({ type: 'broadcast', data: { pad: 'x'.repeat(16 * 1024) } });
  assert.equal(loud.last('error').code, 'too_large');
  assert.ok(loud.closedByServer);
});

test('losing the host closes the game', () => {
  const host = connect();
  const joiner = connect();
  host.say({ type: 'advertise', name: 'game', stat: '', state: 0, max_players: 4 });
  const game = host.last('advertised').game;
  joiner.say({ type: 'join', game });

  host.hangUp();

  assert.equal(joiner.last('game_closed').game, game);
  assert.equal(server.registry.rooms.size, 0);
});
