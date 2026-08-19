// Rules the directory has to get right, tested without a socket.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import * as protocol from '../src/protocol.js';
import { Registry, roomId } from '../src/rooms.js';

const refused = code => error => error instanceof protocol.ProtocolError && error.code === code;

function host(reg, { product = 'SEXP', version = 1, name = 'host' } = {}) {
  const peer = reg.addPeer(product, version, name);
  const room = reg.advertise(peer, { name: 'a game', stat: 'stats', state: 0, maxPlayers: 8, extra: '' });
  return [peer, room];
}

test('a peer id is sixteen bytes', () => {
  const peer = new Registry().addPeer('SEXP', 1, 'someone');
  assert.equal(Buffer.from(peer.id, 'hex').length, protocol.PEER_ID_BYTES);
});

test('products are isolated: Diablo and StarCraft never see each other', () => {
  const reg = new Registry();
  host(reg, { product: 'SEXP' });
  assert.deepEqual([...reg.visible(reg.addPeer('DRTL', 1, 'diablo player'))], []);
});

test('versions are isolated', () => {
  const reg = new Registry();
  host(reg, { version: 1 });
  assert.deepEqual([...reg.visible(reg.addPeer('SEXP', 2, 'patched'))], []);
});

test('members of a game may signal each other', () => {
  const reg = new Registry();
  const [hoster, room] = host(reg);
  const joiner = reg.addPeer('SEXP', 1, 'joiner');
  reg.join(joiner, room.id);
  assert.equal(reg.maySignal(joiner, hoster.id).id, hoster.id);
  assert.equal(reg.maySignal(hoster, joiner.id).id, joiner.id);
});

test('a host may be signalled before joining: a lister is a member of nothing yet', () => {
  const reg = new Registry();
  const [hoster] = host(reg);
  const stranger = reg.addPeer('SEXP', 1, 'stranger');
  assert.equal(reg.maySignal(stranger, hoster.id).id, hoster.id);
});

test('signalling does not cross products', () => {
  const reg = new Registry();
  const [hoster] = host(reg, { product: 'SEXP' });
  const outsider = reg.addPeer('DRTL', 1, 'outsider');
  assert.throws(() => reg.maySignal(outsider, hoster.id), refused('wrong_product'));
});

test('an unrelated idle peer may not be signalled', () => {
  const reg = new Registry();
  const a = reg.addPeer('SEXP', 1, 'a');
  const b = reg.addPeer('SEXP', 1, 'b');
  assert.throws(() => reg.maySignal(a, b.id), refused('not_reachable'));
});

test('an open peer may be signalled without a room: DevilutionX players say they are open', () => {
  const reg = new Registry();
  const a = reg.addPeer('DVLX', 1, 'a', { open: true });
  const b = reg.addPeer('DVLX', 1, 'b', { open: true });
  assert.equal(reg.maySignal(a, b.id).id, b.id);
  assert.equal(reg.maySignal(b, a.id).id, a.id);
});

test('open is one-way and still scoped', () => {
  const reg = new Registry();
  const opened = reg.addPeer('DVLX', 1, 'open', { open: true });
  const closed = reg.addPeer('DVLX', 1, 'closed');
  assert.equal(reg.maySignal(closed, opened.id).id, opened.id);
  assert.throws(() => reg.maySignal(opened, closed.id), refused('not_reachable'));
  const outsider = reg.addPeer('SEXP', 1, 'outsider');
  assert.throws(() => reg.maySignal(outsider, opened.id), refused('wrong_product'));
});

test('relaying needs an open peer or a co-member; hosting is not enough', () => {
  const reg = new Registry();
  const [hoster, room] = host(reg);
  const stranger = reg.addPeer('SEXP', 1, 'stranger');
  assert.throws(() => reg.mayRelay(stranger, hoster.id), refused('not_reachable'));
  reg.join(stranger, room.id);
  assert.equal(reg.mayRelay(stranger, hoster.id).id, hoster.id);
  assert.equal(reg.mayRelay(hoster, stranger.id).id, stranger.id);

  const opened = reg.addPeer('SEXP', 1, 'open', { open: true });
  assert.equal(reg.mayRelay(hoster, opened.id).id, opened.id);
  assert.throws(() => reg.mayRelay(reg.addPeer('DRTL', 1, 'outsider'), opened.id),
                refused('wrong_product'));
});

test('a multicast reaches every other open peer in scope', () => {
  const reg = new Registry();
  const sender = reg.addPeer('DVLX', 1, 'sender', { open: true });
  const listener = reg.addPeer('DVLX', 1, 'listener', { open: true });
  reg.addPeer('DVLX', 1, 'closed');
  reg.addPeer('DVLX', 2, 'other version', { open: true });
  reg.addPeer('DRTL', 1, 'other product', { open: true });
  assert.deepEqual([...reg.openPeers(sender)], [listener.id]);
});

test('relayed payloads are bounded, strict base64', () => {
  assert.equal(protocol.payload({ data: 'aGVsbG8=' }), 'aGVsbG8=');
  for (const bad of ['not base64!', '', 42, 'aGVsbG8'])
    assert.throws(() => protocol.payload({ data: bad }), refused('bad_field'), String(bad));
  const tooBig = 'A'.repeat(Math.ceil((protocol.MAX_RELAY_BYTES + 3) / 3) * 4 + 4);
  assert.throws(() => protocol.payload({ data: tooBig }), refused('bad_field'));
});

test('a game fills up', () => {
  const reg = new Registry();
  const hoster = reg.addPeer('SEXP', 1, 'host');
  const room = reg.advertise(hoster, { name: 'duel', stat: '', state: 0, maxPlayers: 2, extra: '' });
  reg.join(reg.addPeer('SEXP', 1, 'second'), room.id);
  assert.throws(() => reg.join(reg.addPeer('SEXP', 1, 'third'), room.id), refused('game_full'));
});

test('a room expires without a heartbeat', () => {
  const reg = new Registry({ roomTtl: -1 }); // everything is already stale
  host(reg);
  assert.equal(reg.expire().length, 1);
  assert.equal(reg.rooms.size, 0);
});

test('an expired room lets go of everyone in it', () => {
  // It used to forget the room and leave its people pointing at it, so that
  // none of them could ever host.
  const reg = new Registry({ roomTtl: -1 });
  const [hoster, room] = host(reg);
  const joiner = reg.addPeer('SEXP', 1, 'joiner');
  reg.join(joiner, room.id);
  const [expired] = reg.expire();
  assert.deepEqual([...expired.members].sort(), [hoster.id, joiner.id].sort(),
                   'returned with its members, to be told');
  assert.deepEqual([hoster.room, hoster.hosting, joiner.room], [null, null, null]);
  reg.advertise(joiner, { name: 'mine', stat: '', state: 0, maxPlayers: 4, extra: '' });
});

test('a room id comes from its host and a nonce', () => {
  const a = '0123456789abcdef0123456789abcdef';
  const b = 'fedcba9876543210fedcba9876543210';
  assert.match(roomId(a, 'x'), /^[0-9a-f]{16}$/);
  assert.equal(roomId(a, 'x'), roomId(a, 'x'), 'stable, so a re-advert finds its room');
  assert.notEqual(roomId(a, 'x'), roomId(a, 'y'), 'two games of one host');
  assert.notEqual(roomId(a, 'x'), roomId(b, 'x'), 'one nonce, two hosts');
});

test('a resumed id is kept', () => {
  const id = '0123456789abcdef0123456789abcdef';
  assert.equal(new Registry().addPeer('SEXP', 1, 'back', { id }).id, id);
});

test('a host whose connection is gone keeps its room, unlisted and unexpired', () => {
  const reg = new Registry({ roomTtl: -1 });
  const [hoster, room] = host(reg);
  hoster.connected = false;
  assert.deepEqual([...reg.visible(reg.addPeer('SEXP', 1, 'lister'))], []);
  assert.deepEqual(reg.expire(), [], 'its grace period decides, not the heartbeat');
  assert.equal(reg.rooms.get(room.id), room);
  hoster.connected = true;
  assert.equal(reg.expire().length, 1);
});

test('joining again changes nothing; joining elsewhere leaves', () => {
  const reg = new Registry();
  const [, first] = host(reg);
  const [, second] = host(reg);
  const joiner = reg.addPeer('SEXP', 1, 'joiner');
  reg.join(joiner, first.id);
  reg.join(joiner, first.id);
  assert.equal(first.members.size, 2);
  reg.join(joiner, second.id);
  assert.equal(first.members.has(joiner.id), false);
  assert.equal(second.members.has(joiner.id), true);
  assert.equal(joiner.room, second.id);
});

test('re-advertising is a heartbeat, not a second room', () => {
  const reg = new Registry();
  const [hoster, room] = host(reg);
  const again = reg.advertise(hoster, { name: 'renamed', stat: 's', state: 1, maxPlayers: 8, extra: '' });
  assert.equal(again.id, room.id);
  assert.equal(again.name, 'renamed');
  assert.equal(reg.rooms.size, 1);
});

test('losing the host takes the room', () => {
  const reg = new Registry();
  const [hoster, room] = host(reg);
  reg.join(reg.addPeer('SEXP', 1, 'joiner'), room.id);
  reg.removePeer(hoster);
  assert.equal(reg.rooms.size, 0);
});

test('text is bytes and survives intact', () => {
  // StarCraft 1.16.1's own stat string, as its StartAdvertisingGame passed it.
  const stat = ',44,,6,,f,,4,5ab5438b,10,,Chaos\rUntitled Scenario\r';
  assert.equal(protocol.cleanText(stat, protocol.MAX_STAT_BYTES, 'stat'), stat);
  // Bytes above 0x7F travel as U+0080..U+00FF and come through untouched.
  assert.equal(protocol.cleanText('Caf\xe9 \xff', 127, 'name'), 'Caf\xe9 \xff');
  assert.equal(protocol.cleanText(' padded\t', 127, 'name'), ' padded\t');
});

test('text loses NUL and is bounded in bytes', () => {
  assert.equal(protocol.cleanText('a\x00b', 127, 'name'), 'ab');
  // One character is one byte of storm's budget.
  assert.equal(protocol.cleanText('\xe9'.repeat(500), protocol.MAX_NAME_BYTES, 'name'),
               '\xe9'.repeat(protocol.MAX_NAME_BYTES));
  assert.throws(() => protocol.cleanText('\x00\x00', 127, 'name'), refused('bad_field'));
  assert.throws(() => protocol.cleanText(' \r ', 127, 'name'), refused('bad_field'));
});

test('product codes are four characters', () => {
  assert.equal(protocol.cleanProduct('sexp'), 'SEXP');
  assert.throws(() => protocol.cleanProduct('STARCRAFT'), refused('bad_field'));
});

test('an empty stat string is legitimate and must not be refused', () => {
  assert.equal(protocol.cleanText('', protocol.MAX_STAT_BYTES, 'stat', { allowEmpty: true }), '');
});
