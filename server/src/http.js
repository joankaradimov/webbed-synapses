// The signalling protocol over plain HTTP, for hosts whose proxies will not
// upgrade a connection to a WebSocket: the server pushes its frames down a
// Server-Sent Events stream, and the client POSTs its own.
//
// A session is one connection as far as SignalServer is concerned --
// something with send() and close() -- so every rule, limit and the grace
// period apply unchanged. It is created by a POST, read by GETs that each
// hold a stream open for a while, and fed by POSTs of newline-separated
// frames (docs/signalling-protocol.md, "Over HTTP").
//
// Streams end by design. Proxies time requests out, so each stream closes
// after `streamSeconds` and the client asks again from where it got to: every
// event is numbered and kept until a later request shows it arrived, so a cut
// stream loses nothing. A proxy that buffers responses would sit on a stream
// until it ended; for those, a client asks with poll=1, and the request is
// answered as soon as there is anything to say -- long polling, through the
// same endpoint.
//
// Everything is a "simple" request in CORS's terms -- GET, or a POST of
// text/plain -- so a browser page on another origin needs no preflight.

import { randomBytes } from 'node:crypto';

import { log } from './log.js';
import { MAX_FRAME_BYTES } from './protocol.js';

// What ws reports for a connection that ended without a close frame, and what
// a session reports when its client stops asking: dropped, not left.
const ABNORMAL_CLOSURE = 1006;
// A stream is kept alive through proxies with a comment this often.
const KEEPALIVE_MS = 10_000;
// A client that has not asked for its events this long after the last stream
// ended is gone, and gets the grace period after it like a dropped socket. It
// asks again at once when a stream ends, so this is slack for a slow network,
// not a schedule.
const IDLE_SECONDS = 15;
// A closed session answers for a little longer, so that a client between
// requests still learns why: replaced, or the server restarting.
const LINGER_MS = 10_000;
// Unread events a session may pile up before its client counts as dead.
const MAX_QUEUED_EVENTS = 2000;
// A POST carries a batch of frames, each within the protocol's own limit.
const MAX_BODY_BYTES = 16 * MAX_FRAME_BYTES;
const MAX_SESSIONS = 10_000;

const SESSION = /^(?:.*\/)?session(?:\/([0-9a-f]{32})(?:\/(events|close))?)?\/?$/;

const CORS = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type',
  'Access-Control-Max-Age': '86400',
};

function reply(res, status, body = null, headers = {}) {
  const text = body === null ? '' : typeof body === 'string' ? body : JSON.stringify(body);
  res.writeHead(status, {
    ...CORS,
    'Cache-Control': 'no-store',
    ...(text ? { 'Content-Type': typeof body === 'string' ? 'text/plain; charset=utf-8'
                                                          : 'application/json' } : {}),
    ...headers,
  });
  res.end(text);
}

function readBody(req, limit) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    req.on('data', chunk => {
      size += chunk.length;
      if (size <= limit)
        chunks.push(chunk);
      else if (size > 8 * limit)
        req.destroy(); // not worth reading to the end to refuse
    });
    // Read to the end before refusing: a client still sending when the
    // connection closes sees a reset, not the 413.
    req.on('end', () => {
      if (size > limit)
        reject(Object.assign(new Error('body too large'), { status: 413 }));
      else
        resolve(Buffer.concat(chunks).toString('utf8'));
    });
    req.on('error', reject);
  });
}

/**
 * A response that says nothing until it ends: what a proxy that buffers does
 * to a stream. For testing that clients notice and fall back to polling.
 */
function holdBack(res) {
  const held = [];
  return {
    writeHead: (...args) => res.writeHead(...args),
    write: text => { held.push(text); },
    end: () => res.end(held.join('')),
    on: (...args) => res.on(...args),
  };
}

class Session {
  constructor(id, server, idleMs) {
    this.idleMs = idleMs;
    this.id = id;
    this.events = [];   // {seq, text}, oldest first, until acknowledged
    this.seq = 0;
    this.stream = null; // {res, poll, keepalive, end}
    this.lastActive = Date.now();
    this.closedWith = null;
    this.closedAt = 0;
    this.connection = server.open(this);
  }

  // -- what SignalServer sees: a connection ---------------------------------

  send(text) {
    if (this.closedWith)
      return;
    if (this.events.length >= MAX_QUEUED_EVENTS) {
      // Nobody is reading. Treat it like a connection that dropped.
      log.warning(`http session ${this.id.slice(0, 8)}: ${MAX_QUEUED_EVENTS} events unread, dropping it`);
      this.close(ABNORMAL_CLOSURE, 'not reading');
      return;
    }
    this.events.push({ seq: ++this.seq, text });
    this.#flush();
  }

  close(code = 1000, reason = '') {
    if (this.closedWith)
      return;
    this.closedWith = { code, reason };
    this.closedAt = Date.now();
    this.#flush();
    // A socket reports its own closing a moment later; so does this.
    setImmediate(() => this.connection.closed(code));
  }

  // -- the HTTP side ----------------------------------------------------------

  /** A client acknowledges everything up to `after` by asking for what follows it. */
  acknowledge(after) {
    while (this.events.length && this.events[0].seq <= after)
      this.events.shift();
  }

  open(response, { poll, streamMs, buffered = false }) {
    this.#endStream(); // a newer request supersedes an older one
    this.lastActive = Date.now();
    const res = buffered ? holdBack(response) : response;
    res.writeHead(200, {
      ...CORS,
      'Content-Type': 'text/event-stream; charset=utf-8',
      'Cache-Control': 'no-cache, no-store, no-transform',
      // nginx, which a shared host's front end often is, buffers responses
      // unless told not to. Ones that ignore this get poll=1.
      'X-Accel-Buffering': 'no',
    });
    res.flushHeaders?.();
    const stream = { res, poll };
    this.stream = stream;
    stream.end = setTimeout(() => this.#endStream(stream), streamMs);
    if (!poll) {
      // Says the stream is live: a client that does not see this soon is
      // behind a proxy that buffers, and falls back to polling.
      res.write(`event: ready\ndata: {"stream":${Math.round(streamMs / 1000)}}\n\n`);
      stream.keepalive = setInterval(() => res.write(': keepalive\n\n'), KEEPALIVE_MS);
    }
    res.on('close', () => {
      if (this.stream === stream)
        this.#endStream(stream);
    });
    this.#flush();
  }

  #flush() {
    const stream = this.stream;
    if (!stream)
      return;
    for (const event of this.events) {
      if (event.seq > (stream.sent ?? 0)) {
        stream.res.write(`id: ${event.seq}\ndata: ${event.text}\n\n`);
        stream.sent = event.seq;
      }
    }
    if (this.closedWith) {
      stream.res.write(`event: close\ndata: ${JSON.stringify(this.closedWith)}\n\n`);
      this.#endStream(stream);
    } else if (stream.poll && stream.sent) {
      // A long poll answers as soon as there is something to answer with;
      // what follows in the same moment rides along.
      setImmediate(() => this.#endStream(stream));
    }
  }

  #endStream(stream = this.stream) {
    if (!stream || this.stream !== stream)
      return;
    this.stream = null;
    clearTimeout(stream.end);
    clearInterval(stream.keepalive);
    this.lastActive = Date.now();
    stream.res.end();
  }

  /** Called by the sweep. True once the session can be forgotten. */
  sweep(now) {
    if (this.closedWith)
      return now - this.closedAt >= LINGER_MS && !this.stream;
    if (!this.stream && now - this.lastActive >= this.idleMs)
      this.close(ABNORMAL_CLOSURE, 'stopped asking');
    return false;
  }
}

export class HttpSessions {
  /**
   * `streamSeconds` is how long a stream stays open, and how long a poll is
   * held, before the client has to ask again. Below the shortest request
   * timeout of anything in between: 25 seconds clears the common 30 and 60.
   */
  /**
   * `buffered` holds every response back until it ends, as a buffering
   * proxy would: for tests.
   */
  constructor(server, { streamSeconds = 25, idleSeconds = IDLE_SECONDS, buffered = false } = {}) {
    this.server = server;
    this.buffered = buffered;
    this.streamMs = streamSeconds * 1000;
    this.idleMs = idleSeconds * 1000;
    this.sessions = new Map();
    this.sweeper = setInterval(() => this.sweep(), Math.min(5000, Math.max(50, this.idleMs / 3)));
    this.sweeper.unref?.();
  }

  sweep(now = Date.now()) {
    for (const [id, session] of this.sessions)
      if (session.sweep(now))
        this.sessions.delete(id);
  }

  /** Ends every session with `code`: a restart, or going away. */
  closeAll(code, reason) {
    clearInterval(this.sweeper);
    for (const session of this.sessions.values())
      session.close(code, reason);
  }

  /** Handles one request. A bug costs that request its answer, not the process. */
  handle(req, res) {
    this.#route(req, res).catch(error => {
      if (error.status) {
        if (!res.headersSent)
          reply(res, error.status, { error: error.message });
        return;
      }
      log.error(`http handler failed: ${error.stack ?? error}`);
      if (!res.headersSent)
        reply(res, 500, { error: 'internal error' });
      else
        res.destroy();
    });
  }

  async #route(req, res) {
    const url = new URL(req.url, 'http://localhost');
    if (req.method === 'OPTIONS') {
      reply(res, 204);
      return;
    }
    const match = SESSION.exec(url.pathname);
    if (!match) {
      // Anything else -- a browser, a health check -- learns what this is.
      if (req.method === 'GET' || req.method === 'HEAD')
        reply(res, 200, 'snps signalling server: WebSocket here, or HTTP at POST .../session\n');
      else
        reply(res, 404, { error: 'not_found' });
      return;
    }
    const [, id, action] = match;

    if (!id) {
      if (req.method !== 'POST') {
        reply(res, 405, { error: 'method_not_allowed' }, { Allow: 'POST' });
        return;
      }
      await readBody(req, MAX_BODY_BYTES); // nothing in it is needed
      if (this.sessions.size >= MAX_SESSIONS) {
        reply(res, 503, { error: 'busy' });
        return;
      }
      const session = new Session(randomBytes(16).toString('hex'), this.server, this.idleMs);
      this.sessions.set(session.id, session);
      log.debug(`http session ${session.id.slice(0, 8)} opened`);
      reply(res, 201, { session: session.id, stream: this.streamMs / 1000 });
      return;
    }

    const session = this.sessions.get(id);
    // A closed one still answers its event stream, which says why it closed.
    if (!session || (session.closedWith && action !== 'events')) {
      reply(res, 404, { error: 'no_such_session' });
      return;
    }

    if (action === 'events') {
      if (req.method !== 'GET') {
        reply(res, 405, { error: 'method_not_allowed' }, { Allow: 'GET' });
        return;
      }
      const after = Number(url.searchParams.get('after') ?? 0);
      if (!Number.isInteger(after) || after < 0 || after > session.seq) {
        reply(res, 400, { error: 'bad_cursor' });
        return;
      }
      session.acknowledge(after);
      session.open(res, { poll: url.searchParams.get('poll') === '1', streamMs: this.streamMs,
                          buffered: this.buffered });
      return;
    }

    if (req.method !== 'POST') {
      reply(res, 405, { error: 'method_not_allowed' }, { Allow: 'POST' });
      return;
    }
    const body = await readBody(req, MAX_BODY_BYTES);
    if (action === 'close') {
      // The client left: no grace period, as for a WebSocket it closed.
      log.debug(`http session ${id.slice(0, 8)} closed by its client`);
      session.close(1000, 'left');
      reply(res, 204);
      return;
    }
    session.lastActive = Date.now();
    // One frame per line. JSON never needs a raw newline, so a frame cannot
    // contain one.
    for (const line of body.split('\n')) {
      if (!line.trim())
        continue;
      try {
        session.connection.message(line);
      } catch (error) {
        log.error(`connection handler failed: ${error.stack ?? error}`);
        session.close(1011, 'internal error');
        break;
      }
      if (session.closedWith)
        break; // refused -- too large, say -- and nothing more is read
    }
    reply(res, 204);
  }
}
