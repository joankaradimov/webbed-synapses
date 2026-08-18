// ICE server configuration handed to clients at hello time.
//
// TURN credentials are minted here rather than compiled into the client, using
// coturn's REST scheme: the username is an expiry timestamp, the password an
// HMAC of it under a shared secret. A leaked game build therefore does not
// become somebody else's free relay -- the worst it yields is a credential
// that expires.

import { createHmac } from 'node:crypto';

export class IceConfig {
  constructor({ stunUrls = [], turnUrls = [], turnSecret = null, turnTtl = 3600 } = {}) {
    this.stunUrls = [...stunUrls];
    this.turnUrls = [...turnUrls];
    this.turnSecret = turnSecret;
    this.turnTtl = turnTtl;
    Object.freeze(this);
  }

  forPeer(peerId) {
    const servers = [];
    if (this.stunUrls.length)
      servers.push({ urls: [...this.stunUrls] });
    if (this.turnUrls.length && this.turnSecret) {
      const expiry = Math.floor(Date.now() / 1000) + this.turnTtl;
      const username = `${expiry}:${peerId}`;
      servers.push({
        urls: [...this.turnUrls],
        username,
        credential: createHmac('sha1', this.turnSecret).update(username).digest('base64'),
        ttl: this.turnTtl,
      });
    }
    return servers;
  }
}
