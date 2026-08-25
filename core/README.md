# core — the transport

Everything that moves bytes between peers, and nothing that knows what a game
is. Both adapters sit on top of this; neither is visible from inside it.

Built as a **static library from source**, not a shipped binary. DevilutionX
compiles it with its flags, the SNP module with ours, so no C++ ever crosses a
binary boundary and there is no ABI to keep. Consumed the way DevilutionX
consumes libzt: a FetchContent shim under `3rdParty/`, pinned to a commit.
Configure with `SNP_BUILD_PROVIDERS=OFF` and `SNP_BUILD_TESTS=OFF` to get the
core alone. The static MSVC runtime is forced only when this is the top-level
project, so the core takes whatever runtime its consumer uses.

Under Emscripten, a consumer that links with pthreads (`-pthread`, hence
`--shared-memory`) has to compile this whole subtree with `-pthread` as well.
wasm-ld refuses any object built without atomics and bulk memory:
"--shared-memory is disallowed by transport.cpp.o because it was not compiled
with 'atomics' or 'bulk-memory' features". That is the consumer's choice, made
for its whole program, so snps does not add `-pthread` itself. The reverse
does link: objects built with `-pthread` into a program without it, tried with
Emscripten 6.0.9 on 2026-09-27. It has not been run, though, so do not build
snps threaded for an unthreaded page to save the trouble. DevilutionX's web
build is threaded (SDL2's `USE_PTHREADS=1`). Its shim appends `-pthread` to
`CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS` before adding snps. It runs without
cross-origin isolation because the game starts no threads.

A native consumer that also links lwIP — DevilutionX does, through libzt —
needs a shared libjuice. The bundled copy is static, and its global
`udp_sendto` collides with lwIP's (`docs/webrtc-stack.md`, risk 4); a DLL's
stays inside it. snps links an installed libjuice whenever
`find_package(LibJuice)` finds one, and the bundled copy otherwise;
`USE_SYSTEM_JUICE=OFF` forces the bundled one. The SNP module always takes the
bundled copy, since it ships as a single DLL. A consumer that renames the
bundled copy's `udp_sendto`, as DevilutionX's shim does, has to check first
that the `juice-static` target exists: it does not when a libjuice is
installed.

The DevilutionX adapter, `protocol_webrtc`, lives in DevilutionX's own tree,
not here.

**The target name `snps_core` is provisional.** It will read strangely inside
DevilutionX, which has nothing to do with SNPs. Rename it before anyone
upstreams it.

## Rules this code lives under

These come from the two hosts and are unpleasant to retrofit. See `AGENT.md` §4
and `docs/webrtc-stack.md`.

1. **Three targets: x86, x64, wasm.** The browser build is not optional — it is
   the capability that justifies the whole thing to DevilutionX.
2. **Stay inside the libdatachannel ∩ datachannel-wasm API.** `datachannel-wasm`
   implements a subset. A compile-for-wasm CI job belongs here from the start,
   not at integration time.
3. **Nothing at static-init time.** No sockets, no threads, no crypto init in a
   global constructor. Storm `LoadLibrary`s and `FreeLibrary`s every `*.snp` in
   the game directory just to enumerate it, so a static initialiser here runs on
   every trip to the network menu for every module present. `rtc::Preload()` is
   optional by design; call it from `spiInitialize`, never from `DllMain`.
4. **C++17.** Sits inside DevilutionX's C++23 and builds under 32-bit MSVC.
   Do not require anything newer.
5. **Two channels per peer connection** — unreliable/unordered and
   reliable/ordered — because the two hosts want opposite things and both are
   right about what they want.
6. **Poll, do not call back into the host.** Both hosts pull: `spiReceive` on
   one side, `protocol::recv` on the other. Queue internally and let them drain.

## What is here

`snps/transport.hpp` is the whole interface: a `Transport` per player, holding
one WebSocket to the signalling server and one peer connection per peer, each
with an unreliable/unordered and a reliable/ordered channel. No libdatachannel
type appears in it. `snps::initialize()` / `shutdown()` wrap the stack's
process-wide setup, and `setLogSink()` routes diagnostics to the host.

Small datagrams can also go through the server instead of a peer connection:
`sendViaServer()` to one peer, `multicast()` to every open peer with the same
product and version. They arrive as `Channel::Server`. That is DevilutionX's
game-list query and its answers, sent before anyone is connected to anyone.
`Options::open` makes a peer reachable that way, and connectable, with no
room (`docs/signalling-protocol.md`).

Decisions worth knowing before changing it:

- **The lower peer id always offers.** The other side, wanting to send first,
  signals a `request` and waits. Two peers sending to each other at once
  therefore never produce crossing offers, and there is no glare handling
  because there is no glare. `docs/signalling-protocol.md` has the payloads.
- **`send()` to a peer with no connection holds the datagram** (up to
  `Options::heldLimit`) and starts connecting. Held datagrams go out in order
  when the channel opens, before anything sent after it — the reliable channel
  promises ordering and a direct send racing past the queue would break that.
- **Channels are matched by label**, not pre-negotiated: `datachannel-wasm`'s
  `DataChannelInit` has no `negotiated` or `id` (see `docs/webrtc-stack.md`).
- **Nothing calls into libdatachannel while holding the transport's mutex**,
  and a dead peer connection is never destroyed inside its own callback — it
  goes to a graveyard that host calls and the ticker empty.
- **The advert is kept alive by the transport**, re-sent every 10 s against the
  server's 30 s expiry. Natively a ticker thread does it; in a browser there is
  no thread to spare, so every public call also ticks.
- **A link that has not opened both channels in 20 s is dropped**, and the next
  `send()` starts over. That is the retry policy; storm retransmits anyway.
  It holds for both roles: a link still asking for an offer is dropped on the
  same clock, however often it has asked.
- **Links are lost, or let go.** `lost()` reports a link that failed, was
  closed from the other side, or never opened. A channel closing counts at
  once, rather than when ICE times out. `disconnect()` and a replaced link are
  not reported. An offerer asked to connect by a peer it still holds an open
  link to takes the link as stale and starts over; otherwise the next `send()`
  after the other side's `disconnect()` could wait a long time.
- **An open channel does not need the server.** `send()` keeps working on
  links already open after the signalling connection is lost, so a server
  restart does not stop a game. Only new connections need the server, and
  those fail until it is back.
- **The server comes back by itself, once it has been reached.** A lost
  connection is retried, the first time after about a second, then backing off
  to half a minute, every delay jittered so a restart's clients do not return
  in step. Status is `Connecting` meanwhile, not `Offline`, and `self()` keeps
  its id. The hello carries the resume token from the last welcome, so the
  server gives the same id back. Then the advert and the joined game are
  re-sent: the room keeps its id because the advert's nonce, picked once per
  game, goes with it. A join is repeated until the server confirms it, since
  the host may still be reconnecting. The very first connection is different:
  if it fails, or the server refuses the hello, the transport goes `Offline`
  and stays there. Storm's `Initialize` must hear at once that the network is
  unavailable, not after a timeout. Natively the WebSocket also pings a silent
  server, so one that vanished without closing anything is noticed within half
  a minute; a browser's WebSocket cannot ping.
- **No time point means "never".** `steady_clock` counts from boot natively
  but from page load in a browser. A zero time point is not "long ago" there,
  so "not yet" is a `std::optional`.

## Status

Implemented and tested on 32-bit MSVC. `tests/harness` runs transports through
a real server (56 checks). They include open peers meeting the way DevilutionX
does, and a server killed mid-game and replaced, which the transports
reconnect to as the same peers. `tests/snp-sim` runs the SNP adapter built on
the core (28 checks). Under Emscripten it builds on datachannel-wasm, with no
source changes. `tests/wasm` runs it in Node, and in a browser against a native
peer, both through a server restart too (`tests/wasm/README.md`). The relayed
path is untested, since it needs a TURN server.
