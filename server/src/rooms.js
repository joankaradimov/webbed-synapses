// The directory: peers, rooms, and the scoping rules between them.
//
// Deliberately free of sockets and timers so it can be tested directly.
// State is in memory and single-process by design -- see server/README.md.
// None of it has to survive a restart: every client puts its own part back
// when it reconnects (docs/signalling-protocol.md, "Reconnecting").

import { createHash, randomBytes } from 'node:crypto';
import { performance } from 'node:perf_hooks';

import * as protocol from './protocol.js';

/**
 * A 128-bit identity, hex encoded on the wire.
 *
 * This is the number that ends up in SNETADDR and in DevilutionX's
 * endpoint::addr, both of which are exactly 16 opaque bytes. It has to be
 * stable for the session and identical for every participant, because storm
 * gossips it player-to-player and then asks a module to send to a peer it has
 * never met (docs/snp-abi.md section 6). A resume token carries it across a
 * reconnect (resume.js).
 */
export function newPeerId() {
  return randomBytes(protocol.PEER_ID_BYTES).toString('hex');
}

/**
 * A room's id: a hash of its host's id and a nonce the host picked for the
 * game.
 *
 * Derived rather than random, so that a host re-advertising after a reconnect,
 * or to a server that restarted and remembers nothing, gets the same room back
 * -- and its players can join it again by the id they already have. The nonce
 * keeps two games from the same host apart.
 */
export function roomId(hostId, nonce = '') {
  return createHash('sha256').update(`${hostId}/${nonce}`).digest('hex').slice(0, 16);
}

/** Seconds on a monotonic clock. */
const now = () => performance.now() / 1000;

export class Peer {
  constructor({ id, product, version, name, open = false }) {
    this.id = id;
    this.product = product;
    this.version = version;
    this.name = name;
    // Reachable by anyone in its scope -- signalled, relayed to, multicast to
    // -- without sharing a room. The exposure of a public LAN; DevilutionX's
    // peers ask for it, storm's do not (docs/signalling-protocol.md).
    this.open = open;
    this.room = null;
    this.hosting = null;
    // False while its connection is gone and its place is being kept for it
    // (server.js, the grace period). Its rooms are kept but not listed.
    this.connected = true;
  }

  get scope() {
    return `${this.product}/${this.version}`;
  }
}

export class Room {
  constructor({ id, host, product, version, name, stat, state, maxPlayers, extra, members }) {
    this.id = id;
    this.host = host;
    this.product = product;
    this.version = version;
    this.name = name;
    this.stat = stat;
    this.state = state;
    this.maxPlayers = maxPlayers;
    this.extra = extra; // base64, <= MAX_EXTRA_BYTES decoded
    this.members = new Set(members);
    this.refreshed = now();
  }

  get scope() {
    return `${this.product}/${this.version}`;
  }

  summary() {
    return {
      game: this.id,
      name: this.name,
      stat: this.stat,
      state: this.state,
      host: this.host,
      players: this.members.size,
      max_players: this.maxPlayers,
      extra: this.extra,
    };
  }
}

/** Every peer and room the server knows about. */
export class Registry {
  constructor({ roomTtl = 30 } = {}) {
    this.peers = new Map();
    this.rooms = new Map();
    this.roomTtl = roomTtl;
  }

  // -- peers ---------------------------------------------------------------

  /** `id` is a resumed one; without it, a new one is minted. */
  addPeer(product, version, name, { open = false, id = newPeerId() } = {}) {
    const peer = new Peer({ id, product, version, name, open });
    this.peers.set(peer.id, peer);
    return peer;
  }

  /**
   * Drop a peer. Returns the rooms whose members should be told.
   *
   * A host leaving takes its room with it: there is nothing left to join. The
   * returned Room keeps its member set even though it is no longer in the
   * registry, because the caller still has to notify those members -- looking
   * the room up again after this returns finds nothing.
   */
  removePeer(peer) {
    const touched = [];
    const hosted = peer.hosting && this.rooms.get(peer.hosting);
    const joined = !hosted && peer.room && this.rooms.get(peer.room);
    if (hosted) {
      this.rooms.delete(hosted.id);
      this.#detach(hosted);
      touched.push(hosted);
    } else if (joined) {
      joined.members.delete(peer.id);
      touched.push(joined);
    }
    this.peers.delete(peer.id);
    peer.room = null;
    peer.hosting = null;
    return touched;
  }

  /** Release everyone still pointing at a room that no longer exists. */
  #detach(room) {
    for (const member of room.members) {
      const other = this.peers.get(member);
      if (!other)
        continue;
      other.room = null;
      if (other.hosting === room.id)
        other.hosting = null;
    }
  }

  // -- rooms ---------------------------------------------------------------

  /**
   * Create or refresh the caller's room. Repeating this is the heartbeat.
   *
   * The same nonce means the same game, and so the same room. A different one
   * means a new game: the old room is closed first, and the caller tells its
   * members.
   */
  advertise(peer, { nonce = '', name, stat, state, maxPlayers, extra }) {
    protocol.require(peer.room === null || peer.room === peer.hosting,
                     'already_joined', 'cannot host while joined to another game');

    const id = roomId(peer.id, nonce);
    if (peer.hosting && peer.hosting !== id)
      this.unadvertise(peer);

    const existing = this.rooms.get(id);
    if (existing) {
      Object.assign(existing, { name, stat, state, maxPlayers, extra });
      existing.refreshed = now();
      return existing;
    }

    const room = new Room({
      id, host: peer.id, product: peer.product,
      version: peer.version, name, stat, state, maxPlayers, extra, members: [peer.id],
    });
    this.rooms.set(room.id, room);
    peer.hosting = room.id;
    peer.room = room.id;
    return room;
  }

  unadvertise(peer) {
    if (!peer.hosting)
      return null;
    const room = this.rooms.get(peer.hosting) ?? null;
    if (room) {
      this.rooms.delete(room.id);
      this.#detach(room);
    }
    peer.hosting = null;
    peer.room = null;
    return room;
  }

  /**
   * Join a room. Joining one the peer is already in changes nothing, which is
   * what a client that reconnected does. Joining another leaves the room it
   * was in, unless it hosts that one.
   */
  join(peer, id) {
    const room = this.rooms.get(id);
    protocol.require(room !== undefined, 'no_such_game', 'game not found');
    protocol.require(room.scope === peer.scope, 'wrong_product',
                     'game is for a different product or version');
    if (room.members.has(peer.id)) {
      peer.room = room.id;
      return room;
    }
    protocol.require(room.members.size < Math.min(room.maxPlayers, protocol.MAX_PEERS_PER_ROOM),
                     'game_full', 'game is full');
    if (peer.room && peer.room !== peer.hosting)
      this.rooms.get(peer.room)?.members.delete(peer.id);
    room.members.add(peer.id);
    peer.room = room.id;
    return room;
  }

  /**
   * Rooms this peer may see: same product and version, not expired, and a
   * host that is connected.
   *
   * Blizzard's own providers filter advertisements by programid and versionid
   * for the same reason (IPX.CPP:265) -- Diablo and StarCraft must not appear
   * in each other's lists. A host whose connection is gone may never come
   * back, so its game is not offered to anyone new until it does.
   */
  *visible(peer) {
    for (const room of this.rooms.values())
      if (room.scope === peer.scope && this.#hostConnected(room))
        yield room;
  }

  /**
   * Drop rooms whose host stopped refreshing them, releasing their members.
   * Returns them, members intact, for the caller to tell.
   *
   * Not the room of a host whose connection is gone: it cannot refresh, and
   * the grace period it is in decides its fate.
   */
  expire() {
    const cutoff = now() - this.roomTtl;
    const dead = [...this.rooms.values()]
      .filter(room => room.refreshed < cutoff && this.#hostConnected(room));
    for (const room of dead) {
      this.rooms.delete(room.id);
      this.#detach(room);
    }
    return dead;
  }

  #hostConnected(room) {
    return this.peers.get(room.host)?.connected !== false;
  }

  // -- routing -------------------------------------------------------------

  /**
   * Signalling is allowed within a scope, to co-members, a host, or an open
   * peer.
   *
   * The host case is what makes joining possible at all: a client that has
   * only seen a directory listing is not a member of anything yet. An open
   * peer asked to be reachable, which is how DevilutionX's players connect
   * with no rooms at all.
   */
  maySignal(sender, targetId) {
    const target = this.#inScope(sender, targetId);
    protocol.require(
      Registry.#coMembers(sender, target) || target.hosting !== null || target.open,
      'not_reachable', 'peer is not in your game, not hosting one, and not open');
    return target;
  }

  /**
   * Relaying a datagram is allowed to an open peer, or to a co-member.
   *
   * Hosting a room is not enough, unlike for signalling: a relayed datagram is
   * a game's own traffic, and should reach only a peer that asked to be
   * reachable or that shares a game with the sender.
   */
  mayRelay(sender, targetId) {
    const target = this.#inScope(sender, targetId);
    protocol.require(target.open || Registry.#coMembers(sender, target),
                     'not_reachable', 'peer is not open and not in your game');
    return target;
  }

  /** Where a multicast goes: every other connected open peer in the sender's scope. */
  *openPeers(sender) {
    for (const peer of this.peers.values())
      if (peer.open && peer.connected && peer.id !== sender.id && peer.scope === sender.scope)
        yield peer.id;
  }

  roomMembers(peer) {
    const room = peer.room && this.rooms.get(peer.room);
    if (!room)
      return [];
    return [...room.members].filter(member => member !== peer.id);
  }

  #inScope(sender, targetId) {
    const target = this.peers.get(targetId);
    protocol.require(target !== undefined, 'no_such_peer', 'unknown peer');
    protocol.require(target.scope === sender.scope, 'wrong_product',
                     'peer belongs to a different product or version');
    return target;
  }

  static #coMembers(a, b) {
    return a.room !== null && a.room === b.room;
  }
}
