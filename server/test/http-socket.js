// The signalling protocol over HTTP, behind the browser's WebSocket interface:
// `open`, `message`, `error` and `close` events, send() and close(). So the
// smoke checks, and anything else written against a WebSocket, run over
// either transport unchanged.
//
// For tests and the smoke tool. The clients that play games have their own
// implementation, in the core (docs/signalling-protocol.md, "Over HTTP").

// A stream that has not said `ready` by now is being held back by a proxy
// that buffers, and the session falls back to long polling.
const READY_MS = 3000;

class Closing extends Event {
  constructor(code, reason) {
    super('close');
    this.code = code;
    this.reason = reason;
  }
}

export class HttpSocket extends EventTarget {
  static CONNECTING = 0;
  static OPEN = 1;
  static CLOSING = 2;
  static CLOSED = 3;

  /** `url` is the server's, http:// or https://. `poll` starts in long-poll mode. */
  constructor(url, { poll = false } = {}) {
    super();
    this.base = url.replace(/\/+$/, '');
    this.poll = poll;
    this.readyState = HttpSocket.CONNECTING;
    this.cursor = 0;
    this.outbox = [];
    this.sending = false;
    this.reader = null;
    this.#start();
  }

  send(text) {
    if (this.readyState !== HttpSocket.OPEN)
      throw new Error('not open');
    this.outbox.push(text);
    this.#pump();
  }

  close(code = 1000, reason = '') {
    if (this.readyState >= HttpSocket.CLOSING)
      return;
    this.readyState = HttpSocket.CLOSING;
    fetch(`${this.base}/session/${this.session}/close`, { method: 'POST', body: '' })
      .catch(() => {})
      .finally(() => this.#closed(code, reason));
  }

  // -- the session -----------------------------------------------------------

  async #start() {
    try {
      const response = await fetch(`${this.base}/session`, { method: 'POST', body: '' });
      if (response.status !== 201)
        throw new Error(`POST /session answered ${response.status}`);
      this.session = (await response.json()).session;
    } catch (error) {
      this.dispatchEvent(Object.assign(new Event('error'), { error }));
      this.#closed(1006, error.message);
      return;
    }
    this.readyState = HttpSocket.OPEN;
    this.dispatchEvent(new Event('open'));
    this.#listen();
  }

  async #listen() {
    while (this.readyState === HttpSocket.OPEN) {
      let ended;
      try {
        ended = await this.#stream();
      } catch (error) {
        if (this.readyState !== HttpSocket.OPEN)
          return;
        this.dispatchEvent(Object.assign(new Event('error'), { error }));
        this.#closed(1006, error.message);
        return;
      }
      if (ended)
        return;
    }
  }

  /** One stream, or one poll. True once the session is over. */
  async #stream() {
    const controller = new AbortController();
    const query = `after=${this.cursor}${this.poll ? '&poll=1' : ''}`;
    const response = await fetch(`${this.base}/session/${this.session}/events?${query}`,
                                 { signal: controller.signal });
    if (response.status === 404) {
      this.#closed(1006, 'no such session');
      return true;
    }
    if (response.status !== 200)
      throw new Error(`GET events answered ${response.status}`);

    let ready = this.poll;
    const timer = this.poll ? null : setTimeout(() => {
      if (!ready) {
        this.poll = true; // buffered: poll from now on
        controller.abort();
      }
    }, READY_MS);

    const decoder = new TextDecoder();
    let buffer = '';
    let event = { type: 'message', data: [] };
    try {
      for await (const chunk of response.body) {
        buffer += decoder.decode(chunk, { stream: true });
        let newline;
        while ((newline = buffer.indexOf('\n')) >= 0) {
          const line = buffer.slice(0, newline).replace(/\r$/, '');
          buffer = buffer.slice(newline + 1);
          if (line === '') {
            if (event.type === 'ready') {
              ready = true;
            } else if (event.type === 'close') {
              const { code, reason } = JSON.parse(event.data.join('\n'));
              this.#closed(code, reason);
              return true;
            } else if (event.data.length) {
              if (event.id !== undefined)
                this.cursor = event.id;
              this.dispatchEvent(Object.assign(new Event('message'),
                                               { data: event.data.join('\n') }));
              if (this.readyState !== HttpSocket.OPEN)
                return true;
            }
            event = { type: 'message', data: [] };
          } else if (!line.startsWith(':')) {
            const colon = line.indexOf(':');
            const field = colon < 0 ? line : line.slice(0, colon);
            const value = colon < 0 ? '' : line.slice(colon + 1).replace(/^ /, '');
            if (field === 'data')
              event.data.push(value);
            else if (field === 'event')
              event.type = value;
            else if (field === 'id')
              event.id = Number(value);
          }
        }
      }
    } catch (error) {
      if (error.name !== 'AbortError')
        throw error;
    } finally {
      clearTimeout(timer);
    }
    return this.readyState !== HttpSocket.OPEN;
  }

  /** One POST at a time, carrying whatever has queued up: frames stay in order. */
  async #pump() {
    if (this.sending)
      return;
    this.sending = true;
    try {
      while (this.outbox.length && this.readyState === HttpSocket.OPEN) {
        const batch = this.outbox.splice(0);
        const response = await fetch(`${this.base}/session/${this.session}`, {
          method: 'POST',
          headers: { 'Content-Type': 'text/plain' },
          body: batch.join('\n'),
        });
        if (response.status === 404) {
          this.#closed(1006, 'no such session');
          return;
        }
      }
    } catch (error) {
      this.dispatchEvent(Object.assign(new Event('error'), { error }));
      this.#closed(1006, error.message);
    } finally {
      this.sending = false;
    }
  }

  #closed(code, reason) {
    if (this.readyState === HttpSocket.CLOSED)
      return;
    this.readyState = HttpSocket.CLOSED;
    this.dispatchEvent(new Closing(code, reason));
  }
}
