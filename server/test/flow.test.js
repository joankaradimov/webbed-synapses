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
    this.closedByServer = false; // the close code, once the server closes it
    this.connection = server.open(this);
  }

  send(text) { this.sent.push(JSON.parse(text)); }
  close(code) { this.closedByServer = code ?? true; }

  say(message) { this.connection.message(JSON.stringify(message)); }
  /** The client closes the connection, with a close frame: it has left. */
  leave() { this.connection.closed(1000); }
  /** The connection ends with no close frame: the network, or a crash. */
  drop() { this.connection.closed(1006); }

  last(type) {
    for (let i = this.sent.length - 1; i >= 0; --i)
      if (this.sent[i].type === type)
        return this.sent[i];
    return null;
  }

  count(type) { return this.sent.filter(message => message.type === type).length; }

  get id() { return this.last('welcome').peer; }
  get resume() { return this.last('welcome').resume; }
}

const ice = new IceConfig({
  stunUrls: ['stun:stun.example.org:3478'],
  turnUrls: ['turn:relay.example.org:3478'],
  turnSecret: 's3cret',
});

let server;
beforeEach(() => {
  server = new SignalServer(ice);
});

function connect({ product = 'SEXP', version = 1, name = 'player', ...extra } = {}, to = server) {
  const socket = new FakeSocket(to);
  socket.say({ type: 'hello', product, version, name, ...extra });
  return socket;
}

const advert = (extra = {}) => ({ type: 'advertise', name: 'game', stat: '', state: 0, max_players: 4, ...extra });

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

test('a host that leaves closes the game at once', () => {
  const host = connect();
  const joiner = connect();
  host.say(advert());
  const game = host.last('advertised').game;
  joiner.say({ type: 'join', game });

  host.leave();

  assert.equal(joiner.last('game_closed').game, game);
  assert.equal(server.registry.rooms.size, 0);
});

// -- reconnecting ------------------------------------------------------------

test('welcome carries a resume token, which brings the id back', () => {
  const first = connect({ name: 'Joan' });
  assert.match(first.resume, /^[0-9a-f]{32}\.[0-9a-f]{32}$/);
  assert.ok(first.resume.startsWith(first.id), 'issued for this id');
  first.drop();

  const again = connect({ name: 'Joan', resume: first.resume });
  assert.equal(again.id, first.id);
  assert.equal(again.resume, first.resume, 'the same token, since it is only the id signed');
});

test('a token outlives its server: any server with the same secret honours it', () => {
  const before = new SignalServer(ice, { secret: 'shared' });
  const after = new SignalServer(ice, { secret: 'shared' });
  const elsewhere = new SignalServer(ice, { secret: 'another' });
  const original = connect({}, before);

  assert.equal(connect({ resume: original.resume }, after).id, original.id, 'a restart keeps the id');
  assert.notEqual(connect({ resume: original.resume }, elsewhere).id, original.id,
                  'another secret mints a new one');
  const forged = `${'0'.repeat(32)}.${'0'.repeat(32)}`;
  assert.notEqual(connect({ resume: forged }, after).id, '0'.repeat(32), 'and a forgery gets nothing');
});

test('a resume token must be a string', () => {
  const peer = connect({ resume: 42 });
  assert.equal(peer.last('error').code, 'bad_field');
  assert.equal(peer.last('welcome'), null);
});

test('a dropped host keeps its game through the grace period', t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const host = connect({ name: 'host' });
  const joiner = connect({ name: 'joiner' });
  host.say(advert({ nonce: 'first' }));
  const game = host.last('advertised').game;
  joiner.say({ type: 'join', game });

  host.drop();
  assert.equal(joiner.last('game_closed'), null, 'nobody is told');
  const lister = connect();
  lister.say({ type: 'list' });
  assert.deepEqual(lister.last('games').games, [], 'but nobody new is offered the game meanwhile');

  t.mock.timers.tick(19_000);
  const back = connect({ name: 'host', resume: host.resume });
  assert.equal(back.id, host.id);
  back.say(advert({ nonce: 'first' }));
  assert.equal(back.last('advertised').game, game, 'its game, under the same id');
  lister.say({ type: 'list' });
  assert.equal(lister.last('games').games[0]?.players, 2, 'listed again, with the joiner still in it');
  back.say({ type: 'signal', to: joiner.id, data: { kind: 'request' } });
  assert.equal(joiner.last('signal').from, back.id, 'and still reachable as a co-member');

  t.mock.timers.tick(5_000);
  assert.equal(joiner.last('game_closed'), null, 'the grace period ended when it came back');
});

test('a host that does not come back in time loses its game', t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const host = connect();
  const joiner = connect();
  host.say(advert());
  const game = host.last('advertised').game;
  joiner.say({ type: 'join', game });

  host.drop();
  t.mock.timers.tick(19_999);
  assert.equal(joiner.last('game_closed'), null);
  t.mock.timers.tick(1);
  assert.equal(joiner.last('game_closed').game, game);
  assert.equal(server.registry.peers.has(host.id), false);
});

test('the newer connection wins', () => {
  const first = connect({ name: 'twice' });
  const second = connect({ name: 'twice', resume: first.resume });
  assert.equal(second.id, first.id);
  assert.equal(first.last('error').code, 'replaced', 'the old connection is told why');
  assert.equal(first.closedByServer, 4000, 'and closed');

  first.say(advert({ name: 'stale' }));
  const lister = connect();
  lister.say({ type: 'list' });
  assert.deepEqual(lister.last('games').games, [], 'it no longer speaks for the peer');

  first.drop();
  assert.ok(server.registry.peers.has(second.id), 'nor does its closing take the peer away');
  second.say({ type: 'list' });
  assert.notEqual(second.last('games'), null);
});

test('the same nonce is the same room, even on another server; another nonce, another game', () => {
  const before = new SignalServer(ice, { secret: 'shared' });
  const host = connect({}, before);
  host.say(advert({ nonce: 'one' }));
  const game = host.last('advertised').game;
  assert.match(game, /^[0-9a-f]{16}$/);
  host.say(advert({ nonce: 'one', state: 2 }));
  assert.equal(host.last('advertised').game, game, 'a refresh');

  const after = new SignalServer(ice, { secret: 'shared' });
  const back = connect({ resume: host.resume }, after);
  back.say(advert({ nonce: 'one' }));
  assert.equal(back.last('advertised').game, game, 'a restarted server derives the same id');

  const joiner = connect({}, after);
  joiner.say({ type: 'join', game });
  back.say(advert({ nonce: 'two' }));
  assert.notEqual(back.last('advertised').game, game);
  assert.equal(joiner.last('game_closed').game, game, 'and the old game closes');
});

test('a member who joins again is not announced again', () => {
  const host = connect();
  const joiner = connect();
  host.say(advert());
  const game = host.last('advertised').game;
  joiner.say({ type: 'join', game });
  joiner.say({ type: 'join', game });
  assert.equal(joiner.count('joined'), 2, 'each join is answered');
  assert.equal(host.count('peer_joined'), 1, 'but the host hears of it once');
  host.say({ type: 'list' });
  assert.equal(host.last('games').games[0].players, 2);
});

test('joining another game leaves the first', () => {
  const first = connect();
  const second = connect();
  const joiner = connect();
  first.say(advert({ name: 'first' }));
  second.say(advert({ name: 'second' }));
  joiner.say({ type: 'join', game: first.last('advertised').game });
  joiner.say({ type: 'join', game: second.last('advertised').game });
  assert.equal(first.last('peer_left').peer, joiner.id);
  joiner.say({ type: 'list' });
  const players = Object.fromEntries(joiner.last('games').games.map(g => [g.name, g.players]));
  assert.deepEqual(players, { first: 1, second: 2 });
});

test('an expired game lets its players go, and tells them', () => {
  const stale = new SignalServer(ice, { roomTtl: -1 }); // everything is already stale
  const host = connect({}, stale);
  const joiner = connect({}, stale);
  host.say(advert());
  const game = host.last('advertised').game;
  joiner.say({ type: 'join', game });

  stale.expire();
  assert.equal(joiner.last('game_closed').game, game);
  assert.equal(host.last('game_closed').game, game, 'the host too: it did not ask for this');
  joiner.say(advert({ name: 'mine now' }));
  assert.equal(joiner.last('error'), null, 'a former member may host');
  host.say(advert());
  assert.equal(host.last('error'), null, 'and so may the former host');
});

test('a shutdown says goodbye to no one', () => {
  const host = connect();
  const joiner = connect();
  host.say(advert());
  joiner.say({ type: 'join', game: host.last('advertised').game });
  server.shutdown();
  host.drop();
  assert.equal(joiner.last('game_closed'), null, 'everyone is about to reconnect elsewhere');
});
