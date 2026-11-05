# tests/harness

Two peers, no game.

Neither host is a usable development environment: one is a 1998 binary with no
symbols, the other needs a full game build and a second machine. Nearly every
transport bug is reachable from a harness that links `snps::core` directly and
connects two peers to each other, and almost none of them are pleasant to find
any other way.

This should exist before either adapter does. Worth covering here rather than in
a game: connection establishment and its failure modes, both channel
reliability modes, teardown, peer loss, and — by forcing
`TransportPolicy::Relay` — the relayed path, which is otherwise almost
impossible to reproduce deliberately.

## Running it

`ctest` runs it, or by hand:

```
node tests/harness/run.mjs --respawn 2 build/msvc-x86/tests/harness/RelWithDebInfo/snps-harness.exe
```

`run.mjs` starts the signalling server, `server/`, on a port the server
picks, and passes `--server ws://127.0.0.1:<port>` to whatever it runs. It
needs `npm ci` done in `server/`. A `.mjs` or `.js` program runs under the same
Node. `--verbose`, after the program, shows the transport's own log.

`--respawn SECONDS` makes `run.mjs` do what an orchestrator does with a service
it keeps running: a server that dies is replaced, that many seconds later, by
a new one on the same port. Every server a run starts gets the same
`--secret`, so a replacement honours its predecessor's resume tokens.

`--http` plays a host whose proxy will not upgrade a connection: the server
refuses WebSockets the way such a proxy does, and the program is given an
`http://` url, so its transports have to find that out and fall back to HTTP
sessions by themselves. `--http-buffered` also holds every response back
until it ends, as a buffering proxy does, so they have to fall back to long
polling as well. ctest runs the harness and the wasm test in all three ways
(`transport-harness-http`, `-http-buffered`, and so on).

The harness covers the StarCraft way of meeting:
- coming online, advertising and listing;
- datagrams sent before a connection exists, and the reply path;
- 200 reliable messages in order, and storm's largest datagram;
- loopback;
- a third peer met by gossip, with both sides sending at once;
- a peer leaving, and withdrawing a game;
- a second `initialize()` after `shutdown()`.

And that a transport which never reached the server, or whose hello was
refused, goes `Offline` at once rather than retrying.

Then the DevilutionX way:
- open peers finding each other by multicast, with the answer through the
  server;
- connecting with no room;
- `disconnect()` and `lost()`;
- links that never open, in either role.

Last, it kills the server mid-game, which `run.mjs` allows by passing its
process id. Open channels have to carry on while nothing new can start. With
`--respawn`, a host and a joiner then have to reconnect by themselves as the
same peers. The game has to be listed again under its old id with the joiner
back in it, and a fresh connection between the two has to go through the new
server, which only works if the joiner joined again.

`snps-echo`, built beside it, is a peer that answers everything. It is for
testing another build of the core against this one; `tests/wasm` runs the
browser's against it.

Not covered: the relayed path, which needs TURN, and peer loss detected by ICE
timeouts, which takes longer than a test should.
