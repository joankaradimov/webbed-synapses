// The protocol over a real connection: the one seam flow.test.js cannot reach,
// since it drives the server through a stand-in. Over a WebSocket, the client
// is Node's own, not the `ws` the server uses, so the two sides are
// independent implementations. Over HTTP -- an http:// or https:// url -- it
// is HttpSocket, which has the same interface.
//
// socket.test.js and http.test.js run this against an in-process server;
// tools/signal-smoke.mjs runs it against any server, running or not.

import { MAX_FRAME_BYTES } from '../src/protocol.js';
import { HttpSocket } from './http-socket.js';

export class Client {
  constructor(socket, label) {
    this.socket = socket;
    this.label = label;
    this.id = null;
    this.inbox = [];
    this.waiting = null;
    this.closed = new Promise(resolve => socket.addEventListener('close', resolve, { once: true }));
    socket.addEventListener('message', event => {
      this.inbox.push(JSON.parse(event.data));
      this.waiting?.();
    });
  }

  /** `options.poll` starts an HTTP session in long-poll mode. */
  static open(url, label, options = {}) {
    const socket = /^https?:/i.test(url) ? new HttpSocket(url, options) : new WebSocket(url);
    return new Promise((resolve, reject) => {
      socket.addEventListener('open', () => resolve(new Client(socket, label)), { once: true });
      socket.addEventListener('error', () => reject(new Error(`${label}: cannot connect to ${url}`)),
                              { once: true });
    });
  }

  send(message) { this.socket.send(JSON.stringify(message)); }

  async recv(expect, timeoutMs = 5000) {
    const deadline = Date.now() + timeoutMs;
    while (this.inbox.length === 0) {
      const left = deadline - Date.now();
      if (left <= 0)
        throw new Error(`${this.label}: nothing arrived, expected ${expect}`);
      await new Promise(resolve => {
        const timer = setTimeout(resolve, left);
        this.waiting = () => { clearTimeout(timer); resolve(); };
      });
      this.waiting = null;
    }
    const message = this.inbox.shift();
    if (expect !== undefined && message.type !== expect)
      throw new Error(`${this.label}: expected ${expect}, got ${JSON.stringify(message)}`);
    return message;
  }

  async hello(product = 'SEXP', version = 1, extra = {}) {
    this.send({ type: 'hello', product, version, name: this.label, ...extra });
    const welcome = await this.recv('welcome');
    this.id = welcome.peer;
    return welcome;
  }

  close() { this.socket.close(); }
}

/** Runs every check against `url`; `check(ok, what)` reports each one. */
export async function smoke(url, check, options = {}) {
  const a = await Client.open(url, 'host', options);
  const b = await Client.open(url, 'joiner', options);
  try {
    const welcome = await a.hello();
    check(welcome.peer.length === 32, 'welcome carries a 128-bit peer id');
    check(Array.isArray(welcome.ice), 'welcome carries ICE config');
    check(typeof welcome.resume === 'string' && welcome.resume.startsWith(`${welcome.peer}.`),
          'welcome carries a resume token for that id');
    await b.hello();
    check(a.id !== b.id, 'peers get distinct ids');

    a.send({ type: 'advertise', name: 'smoke game', stat: '1v1', state: 0, max_players: 4 });
    const game = (await a.recv('advertised')).game;
    check(game.length === 16, 'advertise returns a game id');

    b.send({ type: 'list' });
    const games = (await b.recv('games')).games;
    const listed = games.find(g => g.game === game);
    check(listed !== undefined, 'the game is listed');
    check(listed?.name === 'smoke game' && listed?.host === a.id, 'the listing carries name and host');

    const c = await Client.open(url, 'diablo', options);
    await c.hello('DRTL');
    c.send({ type: 'list' });
    const other = (await c.recv('games')).games;
    check(!other.some(g => g.game === game), 'a different product does not see the game');
    c.close();

    b.send({ type: 'join', game });
    const joined = await b.recv('joined');
    check(joined.host === a.id && joined.peers.length === 1 && joined.peers[0] === a.id,
          'join reports the host and the existing members');
    check((await a.recv('peer_joined')).peer === b.id, 'the host is told about the joiner');

    const offer = { sdp: 'v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\n', type: 'offer' };
    b.send({ type: 'signal', to: a.id, data: offer });
    const relayed = await a.recv('signal');
    check(relayed.from === b.id && JSON.stringify(relayed.data) === JSON.stringify(offer),
          'signalling payloads relay verbatim');

    a.send({ type: 'broadcast', data: { hello: 'everyone' } });
    const heard = await b.recv('broadcast');
    check(heard.from === a.id && heard.data.hello === 'everyone', 'broadcast reaches the other member');

    b.send({ type: 'nonsense' });
    check((await b.recv('error')).code === 'unknown_type', 'an unknown message type is rejected');

    const d = await Client.open(url, 'rude', options);
    d.send({ type: 'signal', to: a.id, data: {} });
    check((await d.recv('error')).code === 'no_hello', 'hello is required first');
    d.close();

    const e = await Client.open(url, 'loud', options);
    await e.hello();
    e.send({ type: 'broadcast', data: { pad: 'x'.repeat(MAX_FRAME_BYTES) } });
    check((await e.recv('error')).code === 'too_large', 'an oversized frame is rejected');
    const closed = await Promise.race([e.closed.then(() => true),
                                       new Promise(resolve => setTimeout(resolve, 2000, false))]);
    check(closed, 'the connection closes after an oversized frame');

    const asker = await Client.open(url, 'asker', options);
    const master = await Client.open(url, 'master', options);
    await asker.hello('DVLX', 1, { open: true });
    await master.hello('DVLX', 1, { open: true });
    asker.send({ type: 'multicast', data: 'cXVlcnk=' });
    const query = await master.recv('multicast');
    check(query.from === asker.id && query.data === 'cXVlcnk=', 'a multicast reaches an open peer');
    master.send({ type: 'relay', to: asker.id, data: 'YW5zd2Vy' });
    const answer = await asker.recv('relay');
    check(answer.from === master.id && answer.data === 'YW5zd2Vy', 'a relay carries the answer back');
    asker.close();
    master.close();

    const leaving = await Client.open(url, 'returning', options);
    const first = await leaving.hello();
    leaving.close();
    await leaving.closed;
    const returning = await Client.open(url, 'returning', options);
    const again = await returning.hello('SEXP', 1, { resume: first.resume });
    check(again.peer === first.peer, 'the token brings the same id back on a new connection');
    returning.close();

    a.close();
    const gone = await b.recv('game_closed');
    check(gone.game === game, 'losing the host closes the game');
  } finally {
    a.close();
    b.close();
  }
}
