// Wire protocol: message names, limits, and the validation that enforces them.
//
// JSON over WebSocket. Signalling payloads are text anyway (SDP is text), the
// volume is tiny, and the browser speaks it natively -- so there is nothing to
// buy by making this binary.
//
// Several limits here are not arbitrary: they come from the shape of storm's
// structs, because whatever the directory stores eventually gets copied into a
// fixed-size field in a 1998 binary. Truncating here is much better than
// truncating there.

// storm's game struct: char szGameName[128] / szGameStatString[128], so 127
// bytes plus a NUL. See docs/snp-abi.md section 5.
export const MAX_NAME_BYTES = 127;
export const MAX_STAT_BYTES = 127;

// SNETSPI_MAXCLIENTDATA is 256, but DirectIP's AdFile only reserves 32 bytes
// for the blob, so 32 is the number that actually works against StarCraft.
export const MAX_EXTRA_BYTES = 32;

export const MAX_FRAME_BYTES = 16 * 1024; // SDP with many candidates, plus headroom
export const MAX_PEERS_PER_ROOM = 16;     // storm's Send fans out to at most 16
export const MESSAGES_PER_SECOND = 30;    // per connection, token bucket

// `relay` and `multicast` carry a game's out-of-band datagrams through the
// server: DevilutionX's game-list query and the answers to it, a few hundred
// bytes each. The cap is what keeps the server from becoming a data relay.
export const MAX_RELAY_BYTES = 1024;
// A multicast is delivered to every open peer in scope, so it has a far
// smaller budget of its own, on top of the connection's.
export const MULTICASTS_PER_SECOND = 4;

export const PEER_ID_BYTES = 16; // == sizeof(SNETADDR) == endpoint::addr

// An advert's nonce is only ever hashed into the room id (rooms.js), so any
// text will do; this only bounds it.
export const MAX_NONCE_CHARS = 64;

// Battle.net product codes are four characters; we do not interpret them, we
// only refuse to let two different games see each other's rooms.
const PRODUCT_RE = /^[A-Za-z0-9]{4}$/;

// Strict base64, padding included. Buffer.from(text, 'base64') alone would
// skip anything it dislikes and decode the rest.
const BASE64_RE = /^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/;

// Text that ends up in one of storm's char[] fields is *bytes*, in whatever
// code page the game uses, and some of those bytes are structure: StarCraft's
// stat string separates its fields with '\r' (observed 2026-09-17). On the
// wire each byte is one character, U+0001 to U+00FF -- the bytes read as
// Latin-1 -- so a JSON string carries them losslessly and one character is one
// byte of storm's budget. The core does the mapping both ways
// (core/src/transport.cpp).
//
// An earlier version stripped control characters here, on the theory that
// they were the hazard. They were the format: stripping '\r' left StarCraft
// with a stat string it could not parse, and it hid the game from every joiner.

/** Malformed or disallowed message. The connection gets an error frame. */
export class ProtocolError extends Error {
  constructor(code, message) {
    super(message);
    this.code = code;
  }
}

export function require(condition, code, message) {
  if (!condition)
    throw new ProtocolError(code, message);
}

const KINDS = {
  string: value => typeof value === 'string',
  integer: value => Number.isInteger(value),
  object: value => value !== null && typeof value === 'object' && !Array.isArray(value),
};

/** A field that must be there and be of one kind: 'string', 'integer' or 'object'. */
export function field(message, name, kind) {
  const value = message[name];
  require(KINDS[kind](value), 'bad_field', `${name} must be ${kind === 'integer' ? 'an' : 'a'} ${kind}`);
  return value;
}

/** An optional boolean, false when absent. */
export function flag(message, name) {
  const value = message[name] ?? false;
  require(typeof value === 'boolean', 'bad_field', `${name} must be true or false`);
  return value;
}

/** An optional number, truncated to an integer; `fallback` when absent. */
export function integer(message, name, fallback) {
  const value = message[name] ?? fallback;
  const number = typeof value === 'string' && value.trim() !== '' ? Number(value) : value;
  require(typeof number === 'number' && Number.isFinite(number), 'bad_field',
          `${name} must be a number`);
  return Math.trunc(number);
}

/** Decodes strict base64, or throws bad_field naming `what`. */
export function base64(text, what) {
  require(typeof text === 'string' && BASE64_RE.test(text), 'bad_field', `${what} must be base64`);
  return Buffer.from(text, 'base64');
}

/**
 * A relayed datagram: base64 of 1 to MAX_RELAY_BYTES bytes.
 * Checked, then passed on exactly as it came. The server never looks inside.
 */
export function payload(message, name = 'data') {
  const value = field(message, name, 'string');
  const raw = base64(value, name);
  require(raw.length > 0 && raw.length <= MAX_RELAY_BYTES, 'bad_field',
          `${name} must be 1 to ${MAX_RELAY_BYTES} bytes`);
  return value;
}

/**
 * Drop NUL and truncate to the bytes storm can hold. Touch nothing else.
 *
 * NUL is the one byte a C string cannot contain. Everything else, whitespace
 * at either end included, may be part of a format the server knows nothing
 * about. Under the byte-per-character convention above, `limit` characters are
 * `limit` bytes; a character beyond U+00FF, from a client that ignores the
 * convention, reaches the core as '?'.
 *
 * Empty is allowed where empty is meaningful -- a game with no stat string is
 * perfectly ordinary, and rejecting it would refuse a legitimate advertisement.
 */
export function cleanText(value, limit, what, { allowEmpty = false } = {}) {
  value = value.replaceAll('\x00', '');
  if (!allowEmpty)
    require(value.trim() !== '', 'bad_field', `${what} must not be empty`);
  // By code point, so a character outside the BMP is never cut in half.
  const characters = Array.from(value);
  return characters.length > limit ? characters.slice(0, limit).join('') : value;
}

export function cleanProduct(value) {
  require(PRODUCT_RE.test(value), 'bad_field', 'product must be a four-character code, e.g. SEXP');
  return value.toUpperCase();
}
