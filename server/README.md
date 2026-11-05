# server — signalling and the game directory

One service, one dependency (`ws`), no database. Node 22 or newer.

```
cd server
npm ci
node src/main.js --port 8765 --secret <any-long-random-string> \
    --stun stun:stun.l.google.com:19302 \
    --turn turn:relay.example.org:3478 --turn-secret <shared-with-coturn>
```

Every option also reads from the environment (`SNPS_HOST`, `SNPS_PORT`,
`SNPS_STUN`, `SNPS_TURN`, `SNPS_TURN_SECRET`, `SNPS_TURN_TTL`, `SNPS_SECRET`,
`SNPS_GRACE`, `SNPS_ROOM_TTL`, `SNPS_HTTP_STREAM`), so the same image runs in
a container unchanged. Without `SNPS_PORT`, a platform's `PORT` is taken. A host that
deploys the repository root rather than this directory gets the root
`package.json`: `npm ci` there installs this one, and `npm start` runs it. `--verbose` logs every frame in and out, truncated, which is how a
failed negotiation gets diagnosed. Run `coturn` beside it; do not write a TURN
server.

`--secret` signs the resume tokens that give a reconnecting client its peer id
back. Give every instance the same one, and a restart or a replacement loses
nobody's identity. Without it the server makes one up, and ids survive a
dropped connection but not a restart; it warns about that when it starts. It
must not be the TURN secret, which coturn holds too. `--grace` (default 20
seconds) is how long a peer whose connection dropped keeps its place.

## Behind a host that will not upgrade

The one port answers WebSocket upgrades and plain HTTP both. Where a host's
proxy will not upgrade a connection — SiteGround's Node.js hosting, as far as
anyone has found, and shared hosting generally — clients speak the same
protocol as HTTP sessions: Server-Sent Events for what the server pushes,
POSTs for what they send (`docs/signalling-protocol.md`, "Over HTTP").
Nothing on the server needs switching on. On such a host:

- **Give clients the `https://` url.** Given `http(s)://`, a client tries a
  WebSocket at the same address first, and uses HTTP if that is refused; given
  `ws(s)://` it tries nothing else. `webbed.ini`'s `signalling = http`, or
  `Options::signalling`, skips the attempt, and with it the line a browser
  logs for a refused WebSocket.
- **Set `--secret`.** A host that restarts the process at will makes resume
  tokens the difference between a blip and everyone's ids changing.
- **`--http-stream`** (default 25 seconds) is how long a stream or a long poll
  stays open before the client asks again. Keep it under the shortest request
  timeout in front of the server. A proxy that buffers responses is handled by
  the clients, which notice within 3 seconds and switch to long polling.
- **Check it from outside:** `node tools/signal-smoke.mjs --url https://…`
  runs the 20 protocol checks over HTTP against the deployment, and `--poll`
  over long polling.

Every answer carries `Access-Control-Allow-Origin: *`, so a browser build
served from anywhere can use it. For testing clients against such hosts,
`--no-websocket` answers an upgrade the way a proxy that does not do
WebSockets does, and `--http-buffered` holds every response back until it
ends, the way a buffering proxy does.

## Testing it

```
npm test                                             # in server/
node tools/signal-smoke.mjs --url ws://host:8765     # from the root
```

`npm test` runs Node's own test runner:
- **49 unit tests.** `rooms.test.js` checks the directory's rules directly.
  `flow.test.js` drives the whole server through a stand-in connection, with
  no network, and mocked timers for the grace period.
- **The protocol over a real socket.** `socket.test.js` runs 20 checks against
  an in-process server, and a restart: a client closed with 1012 comes back to
  a second server as the same peer, with the same game. The client is Node's
  built-in `WebSocket`, not the `ws` the server uses, so the two ends are
  independent implementations. That is the one seam the stand-in cannot reach.
- **The protocol over HTTP sessions.** `http.test.js` runs the same 20 checks
  streaming and again long polling, through `test/http-socket.js`, a client
  with the browser's WebSocket interface over `fetch()`. Then a WebSocket
  host and an HTTP joiner in one game, a cut stream that loses nothing, a
  client that stops asking, a restart, a replacement, and the plumbing: 404s,
  CORS, cursors, oversized bodies.

`tools/signal-smoke.mjs` runs the same 20 checks against any server: a
deployment with `--url` (`ws(s)://` or `http(s)://`), or one it starts itself.

The transport tests elsewhere in the repository start this server themselves,
through `tests/harness/run.mjs`. The harness's last section kills it, and
`run.mjs --respawn` starts another in its place, for the transports to come
back to.

## Why Node

The workload is small. **No game data passes through this service**, beyond
`relay` and `multicast`: a few hundred bytes each, capped. It relays signalling
text and answers directory queries, while the game's bytes go peer-to-peer or
through TURN. A player costs a handful of messages to join, one heartbeat every
ten seconds while hosting, and a burst of ICE candidates per connection. That is
an I/O-bound problem measured in messages per minute, which suits an event loop,
and the service stays short enough to read in one sitting.

Node was already a dependency: Emscripten runs on it, and the browser build's
test runs in it. JavaScript is also the language of the browser clients this
server now serves. `ws` is Node's standard WebSocket server, with no
dependencies of its own.

Until 2026-10-01 this was Python, on asyncio and `websockets`. The port kept
the wire protocol, the limits and the behaviour. That included two directory
bugs listed under "Loose ends" in `AGENT.md`; the one in room expiry was fixed
the same day.

State is in memory, and none of it is precious. Clients own their part and put
it back after a reconnect, so a restart, or a replacement instance, costs a few
seconds of reconnecting and nothing else (`docs/signalling-protocol.md`,
"Reconnecting"). Scaling out is another matter. Two instances at once would
each hold only the rooms created on it, so every member of a room must reach
the same one: route by product and version, or move room state somewhere
shared first. If that day comes, the boundary is already drawn. `rooms.js`
holds all of it and knows nothing about sockets or timers.

## Layout

```
src/protocol.js   message limits and validation
src/rooms.js      peers, rooms, scoping -- pure logic, no I/O
src/resume.js     resume tokens: a peer id, signed, verifiable by any instance
src/ice.js        STUN/TURN config and coturn REST credentials
src/server.js     the connection handler, and run(), which serves it over ws and http
src/http.js       HTTP sessions: SSE down, POSTs up, for hosts that will not upgrade
src/log.js        timestamped lines on stderr; --verbose adds every frame
src/main.js       configuration and entry point
test/             node --test: rooms, flow (stand-in), socket and http (real),
                  smoke.js, and http-socket.js, the HTTP client the checks use
```

`SignalServer` does not know the WebSocket library. A connection is anything
with `send()` and `close()`. `run()` wires `ws` to it, and the tests wire a
stand-in, so the rules and the routing can be tested without a socket in
sight.

## Notes that are not obvious

**Limits come from storm's structs.** `MAX_NAME_BYTES` is 127 because storm's
game struct holds `char szGameName[128]`; `MAX_EXTRA_BYTES` is 32 because that
is what DirectIP's `AdFile` reserves, even though `SNETSPI_MAXCLIENTDATA` says
256. Truncating here beats truncating inside a 1998 binary.

**Names and stat strings are someone else's bytes.** One character per byte
(U+0001–U+00FF) on the wire; the server drops NUL and truncates, nothing more.
StarCraft's stat string uses CR as a field separator, and stripping it once
made the game hide every advertised game from joiners
(`docs/signalling-protocol.md`).

**Rooms are scoped by product and version.** Blizzard's own providers filter
advertisements the same way (`IPX.CPP:265`) so that different titles never
appear in each other's game lists.

**The connection stays open for the whole session.** It is not a join-time
handshake. Storm gossips peer addresses between players and then asks a module
to reach someone it has never met, so an introduction can be needed at any
moment. The server pings every 20 seconds, which keeps NATs from forgetting
the connection. A client that has not answered one by the next is cut off, and
gets the grace period like any connection that drops.

**A restart is routine.** SIGTERM closes every connection with 1012, "service
restart", and the clients come back to whatever is listening next. They resume
their ids with the tokens they hold, and hosts and members re-send their
adverts and joins. A room id is derived from its host's id and the advert's
nonce, so the room comes back under the id its members already have. Nothing
is said to anyone on the way down: no `game_closed`, no `peer_left`.

**Some peers are open.** DevilutionX's players find games the way they would on
a LAN: a query to everyone, answered by whoever is running each game. A peer
that says `open` in its `hello` can be signalled, sent a `relay` and reached by
a `multicast` from anyone in its scope, with no room involved. `relay` and
`multicast` carry at most 1 KB, and a `multicast` is limited to 4 a second,
since each one fans out (`docs/signalling-protocol.md`). `broadcast`, which
only reaches a room, predates them and has no users.

**A bug in one connection's handling costs that connection, not the process.**
`run()` catches whatever a message handler throws, logs it and closes that
socket. In Node, an exception left to escape a socket's event would end the
server for every player.
