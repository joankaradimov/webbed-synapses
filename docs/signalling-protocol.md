# The signalling protocol

JSON text frames over one connection per peer, held open for the whole session:
a WebSocket, or, behind a host that will not upgrade one, an HTTP session
carrying the same frames ("Over HTTP", below). Implemented in `server/`; see
`server-side.md` for why it is shaped this way.

Every frame is a JSON object with a `type`. Frames are capped at 16 KB and 30
messages per second per connection. Anything malformed draws an `error` and
leaves the connection open — a client's mistake is not fatal.

## Identity

`hello` mints a **128-bit peer id**, hex on the wire, raw bytes in the adapters.
That is exactly `sizeof(SNETADDR)` and exactly DevilutionX's `endpoint::addr`,
which is not a coincidence: storm gossips these between players and then asks a
module to send to a peer it has never met, so the id has to be globally stable
and resolvable rather than an address (`snp-abi.md` §6). It outlives the
connection that minted it: see "Reconnecting".

## Client → server

| Type | Fields | Effect |
|---|---|---|
| `hello` | `product` (4 chars), `version` (int), `name`, `open` (bool, optional), `resume` (optional) | Mints a peer id, or gives back the one `resume` was issued for. Returns `welcome`. Required first. |
| `advertise` | `name`, `stat`, `state`, `max_players`, `extra` (base64 ≤32 B), `nonce` (optional, ≤64 chars) | Creates the room, or refreshes it — **repeating this is the heartbeat**. Returns `advertised`. |
| `unadvertise` | — | Closes the room. |
| `list` | — | Returns `games`, scoped to the caller's product and version. |
| `join` | `game` | Joins the room, leaving any other room the caller is in but does not host. Joining a room again changes nothing. Returns `joined`. |
| `signal` | `to`, `data` | Relays `data` verbatim to a peer. The server never parses it. |
| `relay` | `to`, `data` (base64, 1–1024 B) | Delivers a datagram to one open peer or co-member. |
| `multicast` | `data` (base64, 1–1024 B) | Delivers a datagram to every other open peer in the caller's scope. |
| `broadcast` | `data` | Sends `data` to every other member of the caller's room. |

## Server → client

| Type | Fields |
|---|---|
| `welcome` | `peer`, `ice` (STUN/TURN servers with minted credentials), `resume` (the token for `peer`) |
| `advertised` | `game` |
| `games` | `games[]`: `game`, `name`, `stat`, `state`, `host`, `players`, `max_players`, `extra` |
| `joined` | `game`, `host`, `peers[]` |
| `peer_joined` / `peer_left` | `peer`, `name` |
| `game_closed` | `game` |
| `signal` / `broadcast` | `from`, `data` |
| `relay` / `multicast` | `from`, `data` (base64, exactly as sent) |
| `error` | `code`, `message` |

## What the core puts in `signal`

The server never reads `data`; this is the convention `snps::core` uses inside
it. Every payload is an object with a `kind`:

| `kind` | Fields | Meaning |
|---|---|---|
| `offer` | `sdp` | Session description from the offerer |
| `answer` | `sdp` | The answer to it |
| `candidate` | `candidate`, `mid` | One ICE candidate, trickled |
| `request` | — | "Please make me an offer" |

**Between any two peers, the one whose id is numerically lower makes the
offer.** A higher-id peer that wants to send first sends `request` instead and
repeats it every two seconds until a connection opens. So two peers that start
sending to each other at the same moment — which storm's gossip makes routine
— never produce crossing offers, and neither side needs glare handling. An
`offer` from a peer that should be answering is ignored.

The offerer creates two data channels, labelled `snps-unreliable`
(unordered, no retransmits) and `snps-reliable`; the answerer tells them apart
by label.

## Rules with reasons

**`name` and `stat` are bytes, one character per byte.** They end up in
storm's `char[128]` fields, in whatever code page the game uses, so on the wire
each byte is a character from U+0001 to U+00FF — the bytes read as Latin-1.
JSON carries that losslessly; the core maps both ways. The server removes NUL,
which no C string can hold, truncates to 127 characters (= 127 bytes), and
**touches nothing else**. It used to strip control characters too, and that
broke StarCraft: its stat string separates fields with CR
(`,44,,6,,f,,4,5ab5438b,10,,Chaos\rUntitled Scenario\r`), and a stat string
without them makes the game drop the listing — observed 2026-09-17. `extra` is
capped at 32 bytes because that is what DirectIP's `AdFile` reserves.
Truncating at the server beats truncating inside a 1998 binary.

**Rooms are scoped by `(product, version)`** for listing *and* for signalling.
Blizzard's own providers filter advertisements the same way (`IPX.CPP:265`) so
different titles never appear in each other's game lists.

**Signalling is permitted to a co-member, to any peer that hosts a room in your
scope, or to any open peer in your scope.** The second clause is what makes
joining possible at all — a client that has only seen a listing is not a member
of anything yet. The third is how open peers meet with no rooms at all.

**`signal` payloads are opaque.** SDP and ICE candidates pass through unread, so
the protocol does not need to change when WebRTC does.

**An open peer is reachable by anyone in its scope.** A peer that says `open` in
its `hello` can be signalled, sent a `relay` and reached by `multicast` by any
peer with the same product and version, without sharing a room: the exposure
of a public LAN, or of ZeroTier's public network. DevilutionX's peers are open,
because its players find games the way they would on a LAN: a query to
everyone, answered by whichever player is running each game at the moment.
StarCraft's peers are not open, and nothing changes for them. `open` is one-way:
it makes that peer reachable, and does not let it reach anyone who is not.

**`relay` and `multicast` carry a game's out-of-band datagrams** — DevilutionX's
`send_oob` and `send_oob_mc`, its game-list query and the answers to it. They
arrive as `Channel::Server` in the core. A `relay` goes to an open peer or a
co-member; hosting a room is enough to be signalled, but not to receive
someone's datagrams. A `multicast` goes to every other open peer in scope,
never to the sender. Both are capped at 1024 bytes, because the server is not
there to carry a game. A `multicast` has its own budget of 4 a second, on top
of the connection's 30, because each one is delivered many times.

**`broadcast` is left over.** It reaches only the sender's room, which is why
DevilutionX's query uses `multicast` instead: the players it has to reach are
in no room. Nothing sends `broadcast` now.

**Losing the host closes the game.** There is nothing left to join, and members
are told rather than left waiting. A host that leaves — its client closes the
connection — closes it at once. One whose connection drops gets the grace
period first (below). A room whose host stops refreshing it for 30 seconds
expires the same way, and its members are told.

## Reconnecting

The server keeps nothing that has to survive it. It can be killed and started
again, or replaced by another instance, mid-game: every client puts its own
part back.

**The id comes back with a resume token.** `welcome` carries `resume`, the peer
id and an HMAC of it under a secret only servers hold (`--secret`, or
`SNPS_SECRET`). A client that reconnects sends it in its `hello`, and any server
holding the same secret gives the same id back, whether or not it has ever
heard of it. A token it cannot verify — another secret, a forgery, a server
started without `--secret`, which makes one up per process — gets a new id
instead, with nothing said: the client sees it in the `welcome`. A token is a
bearer credential for its id, so serve `wss://` anywhere but a test.

**The newer connection wins.** A `hello` that resumes an id still connected
elsewhere takes it over: the old connection is sent `error` `replaced` and
closed with code 4000. The client has usually given up on that connection
already, and the server has not noticed yet. A client still using it should
take `replaced` as final rather than resume again, or the two would take turns
evicting each other.

**A dropped connection keeps its place for a while.** When a connection ends
with no close frame — the network dropped it, or the process died — its peer
keeps its id, its rooms and its memberships for the grace period, 20 seconds by
default (`--grace`). Nobody is told anything; its rooms are kept but not
listed, and they do not expire meanwhile. Back in time with its token, it
carries on as if nothing happened. If not, it is dropped as if it had left. A
client that closes the connection itself has left, and is dropped at once.

**The client re-sends its state.** After a reconnect, the host re-sends its
`advertise`, and a member its `join`. Both are harmless to repeat. A new server
that never heard of the room gets it back under **the same id**, because a room
id is derived from the host's peer id and the advert's `nonce`. The host picks
the nonce once per game and keeps it until it withdraws the game. So a member
can join again by the id it already has. Its host may still be reconnecting, so
a `join` refused with `no_such_game` is worth repeating for a while. The core
repeats it every 3 seconds for up to a minute. It stops early when the server
confirms, or says the game closed.

**Shutting down says so.** On SIGTERM the server closes every connection with
1012, "service restart"; on SIGINT with 1001, "going away". Either way clients
reconnect, and nobody is sent `game_closed` or `peer_left`: the server is
about to forget them, not to lose them.

**What is lost is minor.** Rate-limit budgets start over. TURN credentials are
minted afresh in the new `welcome`, which also renews ones about to expire.
Rooms come back as their hosts re-advertise, so a listing can be briefly
incomplete after a restart.

**Running several instances at once** would take one more step: every member
of a room has to reach the same instance, since rooms live in its memory.
Routing connections by product and version would do it. Otherwise the rooms
would have to move to a shared store, such as Redis. Nothing needs that yet.

## Over HTTP

Some hosts' proxies will not upgrade a connection to a WebSocket: shared
hosting often, SiteGround's Node.js hosting among them as far as anyone has
found. The same port therefore also speaks the protocol over plain HTTP. An
**HTTP session** is one connection: everything above — frames, limits, rate,
the grace period, resume tokens — applies unchanged, because to the server it
is just another thing with `send()` and `close()`.

| Request | Answer |
|---|---|
| `POST …/session` | `201 {"session": ID, "stream": 25}`: a new session, ID being 32 hex digits |
| `GET …/session/ID/events?after=N` | `text/event-stream`: the server's frames after event N |
| `GET …/session/ID/events?after=N&poll=1` | the same as a long poll: answered as soon as there is anything, or after 25 s with nothing |
| `POST …/session/ID` | the client's frames, one per line; `204` |
| `POST …/session/ID/close` | the client leaves: no grace period; `204` |

`…` is any path, so the server can sit under a prefix. An unknown or closed
session is `404`, which a client treats as a connection that closed.

**Events are numbered and kept until acknowledged.** Each frame is an SSE event
with `id: N`. A client acknowledges everything up to N by asking for what
follows it, so a stream cut short loses nothing: the next request gets back
whatever had not arrived, then what is new.

**Streams end by design.** Each stays open for `--http-stream` seconds, 25 by
default, then ends, and the client asks again from where it got to. That keeps
every stream under the request timeouts of whatever proxies are in between.
While one is open the server sends a comment every 10 seconds, which keeps
idle-connection timers from cutting it.

**A proxy that buffers turns a stream into silence.** A stream opens with
`event: ready`. A client that has not seen it within 3 seconds is behind a proxy
that holds responses back until they end. It drops that stream and polls from
then on: `poll=1` answers as soon as there is something, so a buffered answer
is a short one. The server also sends `X-Accel-Buffering: no`, which turns
nginx's buffering off where nginx honours it.

**`event: close` ends a session from the server's side,** with
`{"code": …, "reason": …}`: 1012 for a restart, 1001 for going away, 4000 for
`replaced`, the same codes a WebSocket would get.

**A client that stops asking is dropped.** A session with no stream open, and
none asked for in 15 seconds since the last ended, is treated as a dropped
connection: the grace period follows, as for a socket that died.

**A browser on another origin needs no preflight.** Every request is a `GET` or
a `POST` of `text/plain`, CORS's "simple" requests, and every answer carries
`Access-Control-Allow-Origin: *`. The session ID is in the URL, not a cookie,
so nothing ambient is shared. It is a bearer credential for the session, like
the resume token for the peer: serve `https://`.

**Which one a client uses.** The core takes the server's URL scheme as the
answer: `ws://` and `wss://` are WebSockets. `http://` and `https://` are tried
as a WebSocket at the same address first, and reached over HTTP if that is not
welcomed within 3 seconds — refused, which a proxy that will not upgrade does
at once, or left hanging. That answer holds for the transport's lifetime, and
so does a finding that streams are buffered. `Options::signalling` settles it
instead of finding out (`snps/transport.hpp`).

## Not settled

- Authentication. There is none, deliberately, for now.
- Whether `list` should push updates rather than being polled. Storm polls its
  own list roughly once a second, so polling matches, but a push would be
  cheaper.
