// Resume tokens: a peer id that any server holding the secret recognises as
// one it issued, after any restart, without remembering anything.
//
// A token is the id and an HMAC of it under the secret. Whichever process
// checks it -- the one that issued it, its replacement after a restart, a
// second instance -- gives the same id back, so a client that lost its
// connection keeps the identity every other player knows it by
// (docs/signalling-protocol.md, "Reconnecting"). Nothing is stored: the token
// is the state.
//
// A token is a bearer credential for its id. It travels only between the
// server and the peer it was issued to, which is one more reason to serve
// wss:// anywhere but a test.

import { createHmac, randomBytes, timingSafeEqual } from 'node:crypto';

const TOKEN_RE = /^([0-9a-f]{32})\.([0-9a-f]{32})$/;

export class Resume {
  #secret;

  /**
   * Without a secret, one of its own, good only for as long as this process
   * lives: tokens then survive a dropped connection, not a restart.
   */
  constructor(secret = null) {
    this.ephemeral = !secret;
    this.#secret = secret ? Buffer.from(secret, 'utf8') : randomBytes(32);
  }

  #mac(id) {
    // 128 bits of HMAC-SHA256, under a label of its own so the secret could
    // never be mistaken for one that signs something else.
    return createHmac('sha256', this.#secret).update(`snps-resume:${id}`).digest().subarray(0, 16);
  }

  token(id) {
    return `${id}.${this.#mac(id).toString('hex')}`;
  }

  /** The peer id `token` was issued for, or null if no server with our secret issued it. */
  verify(token) {
    const match = typeof token === 'string' ? TOKEN_RE.exec(token) : null;
    if (!match)
      return null;
    return timingSafeEqual(this.#mac(match[1]), Buffer.from(match[2], 'hex')) ? match[1] : null;
  }
}
