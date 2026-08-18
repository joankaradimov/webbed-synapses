// The signalling and directory service.
//
// One WebSocket per peer, held open for the whole session -- not a join-time
// handshake. Storm gossips peer addresses between players and then asks a
// module to reach someone it has never met, so an introduction can be needed
// at any moment (docs/snp-abi.md section 6).
//
// No game data passes through here. Signalling payloads are relayed verbatim
// and never parsed; media and data go peer-to-peer, or through TURN if they
// must. The one exception is `relay` and `multicast`: a few hundred bytes of
// out-of-band datagrams each, capped (protocol.js).
//
// SignalServer knows nothing of the WebSocket library. A connection is
// anything with send(text) and close(); `run` below wires `ws` to it, and the
// tests wire a stand-in.

import { performance } from 'node:perf_hooks';

import { WebSocketServer } from 'ws';

import { log } from './log.js';
import * as protocol from './protocol.js';
import { Registry } from './rooms.js';

/** Token bucket, one per connection. */
class Bucket {
  constructor(rate) {
    this.rate = rate;
    this.tokens = rate;
    this.stamp = performance.now() / 1000;
  }

  take() {
    const now = performance.now() / 1000;
    this.tokens = Math.min(this.rate, this.tokens + (now - this.stamp) * this.rate);
    this.stamp = now;
    if (this.tokens < 1)
      return false;
    this.tokens -= 1;
    return true;
  }
}

const brief = text => (text.length > 200 ? `${text.slice(0, 200)}... [${text.length}]` : text);

export class SignalServer {
  constructor(ice, { roomTtl = 30 } = {}) {
    this.registry = new Registry({ roomTtl });
    this.ice = ice;
    this.sockets = new Map();
    this.multicastBudgets = new Map();
  }

  // -- plumbing --------------------------------------------------------------

  #send(peerId, message) {
    const socket = this.sockets.get(peerId);
    if (!socket)
      return;
    const text = JSON.stringify(message);
    log.debug(`> ${peerId.slice(0, 8)} ${brief(text)}`);
    socket.send(text);
  }

  /**
   * Fan a message out to a member set.
   *
   * Takes the members rather than a room id on purpose: a room is often
   * already gone from the registry by the time the people in it need to hear
   * about it.
   */
  #notify(members, message, { exclude = null } = {}) {
    for (const member of [...members])
      if (member !== exclude)
        this.#send(member, message);
  }

  static #fail(socket, code, message) {
    socket.send(JSON.stringify({ type: 'error', code, message }));
  }

  // -- connection lifecycle --------------------------------------------------

  /**
   * Starts serving one connection. Feed it with the returned object's
   * message(raw) for every frame, and closed() once, when the socket closes.
   */
  open(socket) {
    let peer = null;
    const bucket = new Bucket(protocol.MESSAGES_PER_SECOND);
    let limited = false;
    let done = false;

    const message = raw => {
      if (done)
        return;
      const text = typeof raw === 'string' ? raw : Buffer.from(raw).toString('utf8');
      const who = peer ? peer.id.slice(0, 8) : '(no hello yet)';
      log.debug(`< ${who} ${brief(text)}`);
      if (Buffer.byteLength(text) > protocol.MAX_FRAME_BYTES) {
        log.warning(`peer ${who}: frame of ${Buffer.byteLength(text)} bytes, closing`);
        SignalServer.#fail(socket, 'too_large', 'frame too large');
        done = true;
        socket.close();
        return;
      }
      if (!bucket.take()) {
        // Once per burst: a client that floods would flood the log too.
        if (!limited)
          log.warning(`peer ${who}: over ${protocol.MESSAGES_PER_SECOND} messages a second, dropping`);
        limited = true;
        SignalServer.#fail(socket, 'rate_limited', 'slow down');
        return;
      }
      limited = false;

      let parsed;
      try {
        parsed = JSON.parse(text);
      } catch {
        log.warning(`peer ${who}: invalid JSON`);
        SignalServer.#fail(socket, 'bad_message', 'invalid JSON');
        return;
      }
      try {
        protocol.require(parsed !== null && typeof parsed === 'object' && !Array.isArray(parsed),
                         'bad_message', 'expected a JSON object');
        peer = this.#dispatch(socket, peer, parsed);
      } catch (error) {
        if (!(error instanceof protocol.ProtocolError))
          throw error;
        // The client is told, but its log may be nowhere near this one.
        const kind = parsed !== null && typeof parsed === 'object' ? parsed.type : undefined;
        log.warning(`peer ${who}: refused ${JSON.stringify(kind)}: ${error.code}: ${error.message}`);
        SignalServer.#fail(socket, error.code, error.message);
      }
    };

    const closed = () => {
      if (done && !peer)
        return;
      done = true;
      if (peer) {
        const leaving = peer;
        peer = null;
        this.#drop(leaving);
      }
    };

    return { message, closed };
  }

  #drop(peer) {
    this.sockets.delete(peer.id);
    this.multicastBudgets.delete(peer.id);
    const hosting = peer.hosting;
    for (const room of this.registry.removePeer(peer)) {
      if (room.id === hosting) {
        // The host is gone, so the game is gone with it.
        log.info(`game ${room.id} ${JSON.stringify(room.name)} closed: its host ${peer.id.slice(0, 8)} left`);
        this.#notify(room.members, { type: 'game_closed', game: room.id }, { exclude: peer.id });
      } else {
        this.#notify(room.members, { type: 'peer_left', peer: peer.id });
      }
    }
    log.info(`peer ${peer.id.slice(0, 8)} disconnected`);
  }

  // -- dispatch --------------------------------------------------------------

  #dispatch(socket, peer, message) {
    const kind = message.type;
    protocol.require(typeof kind === 'string', 'bad_message', 'missing type');

    if (kind === 'hello') {
      protocol.require(peer === null, 'already_hello', 'already introduced');
      return this.#hello(socket, message);
    }

    protocol.require(peer !== null, 'no_hello', 'send hello first');

    const handlers = {
      advertise: this.#advertise,
      unadvertise: this.#unadvertise,
      list: this.#list,
      join: this.#join,
      signal: this.#signal,
      broadcast: this.#broadcast,
      relay: this.#relay,
      multicast: this.#multicast,
    };
    // Own keys only: "toString" is not a message type.
    protocol.require(Object.hasOwn(handlers, kind), 'unknown_type',
                     `unknown type ${JSON.stringify(kind)}`);
    handlers[kind].call(this, peer, message);
    return peer;
  }

  #hello(socket, message) {
    const product = protocol.cleanProduct(protocol.field(message, 'product', 'string'));
    const version = protocol.field(message, 'version', 'integer');
    const name = protocol.cleanText(protocol.field(message, 'name', 'string'),
                                    protocol.MAX_NAME_BYTES, 'name');
    const open = protocol.flag(message, 'open');
    const peer = this.registry.addPeer(product, version, name, { open });
    this.sockets.set(peer.id, socket);
    this.multicastBudgets.set(peer.id, new Bucket(protocol.MULTICASTS_PER_SECOND));
    this.#send(peer.id, { type: 'welcome', peer: peer.id, ice: this.ice.forPeer(peer.id) });
    log.info(`peer ${peer.id.slice(0, 8)} joined as ${JSON.stringify(name)} (${product}/${version}${open ? ', open' : ''})`);
    return peer;
  }

  #advertise(peer, message) {
    const extra = Object.hasOwn(message, 'extra') ? protocol.field(message, 'extra', 'string') : '';
    if (extra) {
      const raw = protocol.base64(extra, 'extra');
      protocol.require(raw.length <= protocol.MAX_EXTRA_BYTES, 'bad_field',
                       `extra exceeds ${protocol.MAX_EXTRA_BYTES} bytes`);
    }

    const previous = peer.hosting ? this.registry.rooms.get(peer.hosting) : undefined;
    const oldState = previous?.state;
    const room = this.registry.advertise(peer, {
      name: protocol.cleanText(protocol.field(message, 'name', 'string'),
                               protocol.MAX_NAME_BYTES, 'name'),
      stat: protocol.cleanText(message.stat ? String(message.stat) : '',
                               protocol.MAX_STAT_BYTES, 'stat', { allowEmpty: true }),
      state: protocol.integer(message, 'state', 0),
      maxPlayers: Math.max(1, Math.min(protocol.integer(message, 'max_players', 8),
                                       protocol.MAX_PEERS_PER_ROOM)),
      extra,
    });
    const hex = `0x${room.state.toString(16)}`;
    if (previous === undefined)
      log.info(`peer ${peer.id.slice(0, 8)} created game ${room.id} ${JSON.stringify(room.name)} (${room.product}/${room.version}, up to ${room.maxPlayers} players, state ${hex})`);
    else if (room.state !== oldState)
      log.info(`game ${room.id} ${JSON.stringify(room.name)} is now state ${hex}`);
    else
      log.debug(`game ${room.id} ${JSON.stringify(room.name)} refreshed`); // the ten-second heartbeat
    this.#send(peer.id, { type: 'advertised', game: room.id });
  }

  #unadvertise(peer) {
    const room = this.registry.unadvertise(peer);
    if (!room)
      return;
    log.info(`peer ${peer.id.slice(0, 8)} withdrew game ${room.id} ${JSON.stringify(room.name)}`);
    this.#notify(room.members, { type: 'game_closed', game: room.id }, { exclude: peer.id });
  }

  #list(peer) {
    this.registry.expire();
    const games = [...this.registry.visible(peer)].map(room => room.summary());
    // Every client asks about once a second, so only when asked for detail.
    log.debug(`peer ${peer.id.slice(0, 8)} listed ${games.length} game(s)`);
    this.#send(peer.id, { type: 'games', games });
  }

  #join(peer, message) {
    const room = this.registry.join(peer, protocol.field(message, 'game', 'string'));
    log.info(`peer ${peer.id.slice(0, 8)} joined game ${room.id} ${JSON.stringify(room.name)} (${room.members.size} player(s) now)`);
    this.#send(peer.id, {
      type: 'joined',
      game: room.id,
      host: room.host,
      peers: [...room.members].filter(member => member !== peer.id).sort(),
    });
    this.#notify(room.members, { type: 'peer_joined', peer: peer.id, name: peer.name },
                 { exclude: peer.id });
  }

  /** Relay an offer, answer or candidate. The payload is never parsed. */
  #signal(peer, message) {
    const target = this.registry.maySignal(peer, protocol.field(message, 'to', 'string'));
    this.#send(target.id, { type: 'signal', from: peer.id, data: message.data ?? null });
  }

  /**
   * To every other member of the sender's room.
   *
   * Written for DevilutionX's send_oob_mc, which now uses `multicast` instead:
   * its game-list query has to reach players who are in no room. No client
   * sends this today.
   */
  #broadcast(peer, message) {
    for (const member of this.registry.roomMembers(peer))
      this.#send(member, { type: 'broadcast', from: peer.id, data: message.data ?? null });
  }

  /** One datagram to one peer, through the server -- DevilutionX's send_oob. */
  #relay(peer, message) {
    const target = this.registry.mayRelay(peer, protocol.field(message, 'to', 'string'));
    const data = protocol.payload(message);
    this.#send(target.id, { type: 'relay', from: peer.id, data });
  }

  /**
   * One datagram to every other open peer in scope -- DevilutionX's
   * send_oob_mc, which is how it finds games.
   */
  #multicast(peer, message) {
    const data = protocol.payload(message);
    protocol.require(this.multicastBudgets.get(peer.id).take(), 'rate_limited',
                     `over ${protocol.MULTICASTS_PER_SECOND} multicasts a second`);
    const audience = [...this.registry.openPeers(peer)];
    log.debug(`peer ${peer.id.slice(0, 8)} multicast to ${audience.length} peer(s)`);
    this.#notify(audience, { type: 'multicast', from: peer.id, data });
  }
}

/**
 * Serves `server` on host:port until close() is called on the result. Port 0
 * picks a free one; `address()` says which.
 */
export async function run(server, { host = '0.0.0.0', port = 8765, roomTtl = 30 } = {}) {
  // Frames are checked against MAX_FRAME_BYTES by the handler, which answers
  // with an error first. The library's own cap only has to be above it.
  const wss = new WebSocketServer({ host, port, maxPayload: 1024 * 1024 });

  // Pings keep NATs from forgetting the connection and find the dead ones:
  // a client that has not answered the last ping by the next is dropped.
  const PING_INTERVAL_MS = 20_000;
  wss.on('connection', socket => {
    const connection = server.open(socket);
    socket.alive = true;
    socket.on('pong', () => { socket.alive = true; });
    socket.on('message', data => {
      // A bug here must cost one connection, not every player's.
      try {
        connection.message(data);
      } catch (error) {
        log.error(`connection handler failed: ${error.stack ?? error}`);
        socket.close(1011, 'internal error');
      }
    });
    socket.on('close', () => {
      try {
        connection.closed();
      } catch (error) {
        log.error(`closing a connection failed: ${error.stack ?? error}`);
      }
    });
    socket.on('error', error => log.debug(`socket error: ${error.message}`));
  });
  const pinger = setInterval(() => {
    for (const socket of wss.clients) {
      if (!socket.alive) {
        socket.terminate();
        continue;
      }
      socket.alive = false;
      socket.ping();
    }
  }, PING_INTERVAL_MS);
  const reaper = setInterval(() => {
    for (const room of server.registry.expire())
      log.info(`room ${room.id} expired`);
  }, (roomTtl / 3) * 1000);

  await new Promise((resolve, reject) => {
    wss.once('listening', resolve);
    wss.once('error', reject);
  });
  const address = wss.address();
  log.info(`listening on ws://${address.address}:${address.port}`);

  return {
    address: () => wss.address(),
    close: () => new Promise(resolve => {
      clearInterval(pinger);
      clearInterval(reaper);
      for (const socket of wss.clients)
        socket.terminate();
      wss.close(() => resolve());
    }),
  };
}
