# server — signalling and the game directory

One service, one dependency (`ws`), no database. Node 22 or newer.

```
cd server
npm ci
node src/main.js --port 8765 \
    --stun stun:stun.l.google.com:19302 \
    --turn turn:relay.example.org:3478 --turn-secret <shared-with-coturn>
```

Every option also reads from the environment (`SNPS_HOST`, `SNPS_PORT`,
`SNPS_STUN`, `SNPS_TURN`, `SNPS_TURN_SECRET`, `SNPS_TURN_TTL`,
`SNPS_ROOM_TTL`), so the same image runs in a container unchanged. `--verbose`
logs every frame in and out, truncated, which is how a failed negotiation gets
diagnosed. Run `coturn` beside it; do not write a TURN server.

## Testing it

```
npm test                                             # in server/
node tools/signal-smoke.mjs --url ws://host:8765     # from the root
```

`npm test` runs Node's own test runner:
- **33 unit tests.** `rooms.test.js` checks the directory's rules directly.
  `flow.test.js` drives the whole server through a stand-in connection, with
  no network.
- **The protocol over a real socket.** `socket.test.js` runs 18 checks against
  an in-process server. The client is Node's built-in `WebSocket`, not the `ws`
  the server uses, so the two ends are independent implementations. That is
  the one seam the stand-in cannot reach.

`tools/signal-smoke.mjs` runs the same 18 checks against any server: a
deployment with `--url`, or one it starts itself.

The transport tests elsewhere in the repository start this server themselves,
through `tests/harness/run.mjs`.

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
the wire protocol, the limits and the behaviour. That includes the two
directory bugs listed under "Loose ends" in `AGENT.md`, which are fixes for
another day.

The honest caveat: state is in memory and the process is single. That is right
for this workload and will stay right for a long time, but scaling out is not a
matter of starting a second copy: room state would have to move somewhere
shared first. If that day comes, the boundary is already drawn. `rooms.js`
holds all of it and knows nothing about sockets or timers.

## Layout

```
src/protocol.js   message limits and validation
src/rooms.js      peers, rooms, scoping -- pure logic, no I/O
src/ice.js        STUN/TURN config and coturn REST credentials
src/server.js     the connection handler, and run(), which serves it over ws
src/log.js        timestamped lines on stderr; --verbose adds every frame
src/main.js       configuration and entry point
test/             node --test: rooms, flow (stand-in), socket (real), smoke.js
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
moment. The server pings every 20 seconds. A client that has not answered one
by the next is dropped, which keeps NATs from forgetting the connection.

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
