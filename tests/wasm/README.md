# tests/wasm

The core compiled to WebAssembly and used the way DevilutionX's web build uses
it: the test's own code has no exceptions, the program is linked with
`-sASYNCIFY`, and every wait is an `emscripten_sleep`, which is what
`SDL_Delay` becomes there. It is built only by an Emscripten build of snps,
which adds this directory in place of the native tests.

```
emcmake cmake -S . -B build/wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/wasm
ctest --test-dir build/wasm
```

On this machine Emscripten is MSYS2's, in the UCRT64 environment: put
`/ucrt64/bin` and `/ucrt64/lib/emscripten` on the `PATH` and set
`BINARYEN_CORES=1`, because the multithreaded `wasm-opt` can hang on exit.

## Two halves

Both halves start the same way: a transport is destroyed while its WebSocket
is still connecting, which DevilutionX does on every create and join. Nothing
may throw. In a browser the console should stay clean too, which only a look
at it can tell: a socket closed before it opens makes browsers log
"WebSocket is closed before the connection is established", and
`cmake/datachannel-wasm-patch.cmake` lets such a socket open first.

**In Node** (`wasm-server-path`, under ctest), against a signalling server that
`tests/harness/run.mjs` starts:
- two open transports come online;
- a multicast arrives, and its answer comes back through the server;
- a peer connection is attempted. Node has no `RTCPeerConnection`, so
  datachannel-wasm throws inside the core, and the core must catch it even
  though this file is compiled without exceptions;
- the server is killed, and `run.mjs --respawn` starts another in its place.
  Both transports have to reconnect by themselves, as the same peers, with no
  thread: only the calls a game makes anyway drive the retries.

**In a browser**, against a native peer: `tests/harness`'s `snps-echo`, built by
the native build, which answers everything. The page finds it by multicast,
then:
- sends 50 reliable messages and gets them back in order;
- uses the unreliable channel;
- disconnects and connects again;
- asks `browser.mjs` to replace the signalling server, then reconnects as the
  same peer, finds the echo peer back under its own id, and connects to it
  again through the new server.

That is the browser's WebRTC against libdatachannel, and the browser's
WebSocket losing its server. `browser.mjs` starts the signalling server, the
echo peer and a web server, and prints the address to open:

```
node tests/wasm/browser.mjs build/wasm/tests/wasm build/msvc-x86/tests/harness/RelWithDebInfo/snps-echo.exe
```

The page ends with "passed" or "FAILED", and its title says the same. Any
uncaught error or abort counts as a failure. `--stun` hands the peers a STUN
server, which exercises peer connections made with ICE servers. The address
need not answer: host candidates still connect on one machine.

Passed on 2026-09-27: Chromium in the Claude desktop app's browser pane,
against `snps-echo` built with MSVC x86, with and without `--stun`. Passed
again on 2026-10-01 with the server restart and the early destruction (13
checks, with a clean console), without `--stun`.

## Getting there took two fixes

- **datachannel-wasm 0.4.0's glue read `Module['HEAPU8']` and
  `Module['HEAPU32']`.** Current Emscripten no longer exports those, so every
  data channel send threw a TypeError. `cmake/datachannel-wasm-patch.cmake`
  patches the fetched sources to use the views directly.
- **The core used a default-constructed `steady_clock` time point to mean
  "never".** That only means "long ago" where the clock counts from boot. In a
  browser it counts from page load, so a connection request made in a page's
  first two seconds never went out. Fixed in `core/src/transport.cpp`.
