# Choosing the WebRTC stack

## The requirement

One transport core, three targets, minimal code changes between them:

| Target | Why |
|---|---|
| **x86 (32-bit)** | the StarCraft `.snp` — storm.dll is 32-bit |
| **x64** | DevilutionX desktop |
| **wasm / Emscripten** | DevilutionX's web build, which has no multiplayer today |

The wasm target settles a question that was still open: **the stack has to be
in-process.** A sidecar process was a reasonable way to keep a modern
dependency out of a 1998 executable, but nothing spawns a process inside a
browser tab. One in-process core it is.

It also rules out anything that cannot exist in a browser, because in the
browser the WebRTC implementation *is* the browser's. What we need is not a
library that compiles to wasm — it is a library whose **API** has a browser-backed
implementation, so the same core code compiles against both.

## Candidates

| | Verdict |
|---|---|
| **libwebrtc** (Google) | No. GN build, enormous, no Emscripten story, 32-bit MSVC is a fight. |
| **libdatachannel** + **datachannel-wasm** | **Recommended.** See below. |
| **libjuice** alone | ICE only — no DTLS, no SCTP, so no data channels. It is a *dependency* of libdatachannel, not an alternative. |
| **Pion** (Go), **str0m** (Rust) | Wrong language for a 32-bit MSVC DLL and for an in-tree DevilutionX dependency. |
| **libpeer** and similar embedded stacks | Small, but no browser-side API twin and much less proven. |

## Why libdatachannel

**The wasm twin is the whole argument.** `datachannel-wasm` is by the same
author and states outright that it "exposes the same API as libdatachannel, and
therefore allows to compile the same C++ code using Data Channels and WebSockets
to WebAssembly for browsers in addition to native targets" — same `rtc`
namespace, browser WebRTC underneath. That is precisely "portable with minimal
code changes", and no other candidate offers it.

**The reliability split we need is a first-class setting.**
`include/rtc/reliability.hpp`, checked against v0.24.5 — which also carries two
deprecated backward-compatibility members not shown here:

```cpp
struct Reliability {
    bool unordered = false;
    optional<std::chrono::milliseconds> maxPacketLifeTime;
    optional<unsigned int> maxRetransmits;
};
```

One peer connection, two channels: `{unordered = true, maxRetransmits = 0}` for
storm, which does its own sequencing and retransmission, and the default
reliable-ordered channel for DevilutionX's `base_protocol::send`. No layering
tricks.

**C++17, and MSVC is a supported first-class target** (`CXX_STANDARD 17`, several
`if(MSVC)` branches in its CMake). C++17 sits comfortably inside DevilutionX's
C++23 and inside a 32-bit MSVC build.

**The dependency footprint is tunable**, which matters given DevilutionX's small
budget: `NO_MEDIA=ON` drops libsrtp, `USE_MBEDTLS=ON` swaps OpenSSL for something
far smaller. That leaves roughly libjuice + usrsctp + Mbed TLS + plog.

**Keep WebSocket support on.** It is tempting to trim, but `datachannel-wasm`
mirrors libdatachannel's WebSocket API too (wrapping the browser's), so the
signalling client is also write-once across native and wasm. Trimming it means
writing that twice.

**Initialization is explicitly lazy.** `include/rtc/global.hpp` offers
`Preload()` and `Cleanup()` as *optional*. This matters more than it looks:
storm `LoadLibrary`s and `FreeLibrary`s every `*.snp` in the game directory
during enumeration, so anything the core does at static-init time happens on
every trip to the network menu. Never call `Preload()` from `DllMain`; call it
from `spiInitialize`. `SetThreadPoolSize` and `SetSctpSettings` give control of
the threading and the SCTP retransmit behaviour, the latter worth tuning against
storm's latency model.

## Licensing

libdatachannel is **MPL 2.0** since 0.18 — file-level copyleft, satisfied by
publishing modifications to its own files. Consumed unmodified through
FetchContent, the obligation is close to nil. `datachannel-wasm` is **MIT**.

This is not a new kind of ask for DevilutionX: `libzt`, which they already ship,
is under the **Business Source License 1.1**. MPL 2.0 is considerably more
permissive than what is already in their tree.

## The 32-bit MSVC build, proven

`tools/webrtc-probe/` is the experiment that closed this question, and it stays
in the tree as the reproduction. It builds the stack the way the core would
consume it and then opens a real peer connection between two in-process peers.

Verified on Windows 11, Visual Studio 17 2022 (`-A Win32`, MSVC 14.44), against
**libdatachannel v0.24.5** with `NO_MEDIA=ON`, `USE_MBEDTLS=ON`, WebSockets kept,
and **Mbed TLS 3.6.7**:

- the whole chain compiles for x86 — mbedcrypto, mbedx509, mbedtls, everest,
  p256-m, usrsctp, libjuice, datachannel — with warnings but no errors
- `sizeof(void*) == 4` in the running binary
- both peers reach `Connected` over loopback ICE, DTLS and SCTP
- both channels open with the reliability the two hosts need: `unordered` with
  `maxRetransmits = 0` for storm, ordered and reliable for DevilutionX, and the
  negotiated `reliability()` reads back as requested on each
- 200 bytes over the unreliable channel and 4000 over the reliable one arrive
  intact, and a reply comes back
- `maxMessageSize()` is 262144 bytes
- `Preload()`/`Cleanup()` survives a second cycle, which is the one storm
  forces by unloading and reloading every module on each visit to the network
  menu

**Two things the build needs that are not obvious.**

`find_package(MbedTLS 3 REQUIRED)` looks for an *installed* Mbed TLS —
libdatachannel vendors plog, usrsctp, libjuice, libsrtp and json as submodules
but no TLS backend. It is guarded by `if(NOT TARGET MbedTLS::MbedTLS)`, so
defining that target first (over a `FetchContent`ed Mbed TLS) skips the search
entirely.

`MBEDTLS_SSL_DTLS_SRTP` must be enabled in Mbed TLS **even with
`NO_MEDIA=ON`**. `src/impl/dtlstransport.cpp:416` calls
`mbedtls_ssl_conf_dtls_srtp_protection_profiles()` unconditionally — that code
is not behind `RTC_ENABLE_MEDIA` — so a stock Mbed TLS build fails on
undeclared `MBEDTLS_TLS_SRTP_*`. `cmake/mbedtls-user-config.h`
turns it on via `MBEDTLS_USER_CONFIG_FILE`, which Mbed TLS propagates as a
PUBLIC compile definition so libdatachannel sees the same configuration.

## Risks, in order

1. ~~**32-bit MSVC is unproven.**~~ Retired — see above. ~~Inside a DLL~~ is
   retired too: `tests/snp-sim` loads two copies of `webbed.snp` into one
   process, connects them, unloads both with their stacks stopped, and loads
   one again. In the main build every dependency links the static CRT, and the
   module imports only `KERNEL32`, `WS2_32`, `IPHLPAPI` and `bcrypt`.
2. **`datachannel-wasm` implements a subset.** The core must stay inside the
   intersection of the two APIs. Worth a compile-for-wasm CI job from the very
   first commit, so drift is caught immediately rather than at integration.
   What its headers allow, checked while writing the core: `DataChannelInit`
   holds **only** `reliability` — no `negotiated`, no `id`, so channels are
   opened in-band and matched by label; `Configuration` holds **only**
   `iceServers` — no `iceTransportPolicy`, so forcing TURN is native-only;
   there is no explicit `setLocalDescription()`, so negotiation is left
   automatic; `IceServer(url)` passes the URL through unparsed, while setting
   `username` and `password` afterwards works on both. `InitLogger`, `Preload`
   and `Cleanup` exist on both. `DataChannel::maxMessageSize()` and
   `PeerConnection::localAddress()` do not.
   **Compiled, 2026-09-27:** the core builds against datachannel-wasm 0.4.0
   unchanged. Its JavaScript did not survive a current Emscripten. It read
   `Module['HEAPU8']` and `Module['HEAPU32']`, which are no longer exported,
   so every data-channel send threw. `cmake/datachannel-wasm-patch.cmake`
   patches it at fetch. Worth reporting upstream. The same patch keeps a
   WebSocket deleted while still connecting from being closed before it
   opens, which browsers log as an error (2026-10-01). Since the JavaScript
   reaches the link only as `--js-library` options, the build also makes
   programs depend on those files, or a change to them would relink nothing.
3. **Emscripten threading.** The browser build's concurrency model differs;
   the core's threading assumptions have to survive it. **As DevilutionX uses
   it, they do.** Everything runs on the page's main thread under
   `-sASYNCIFY`, with no pthreads; the browser's WebRTC lives on that thread.
   Its events arrive when the program yields in `emscripten_sleep`. The core
   runs no thread there and does its housekeeping on every call.
   `waitOnline()` blocks without yielding, so it cannot be used. A related
   surprise: `steady_clock` counts from page load, not boot, so a zero time
   point is not "long ago". The core had used one to mean "never" and lost a
   page's first connection request to it. A consumer linked with pthreads has
   to compile snps with `-pthread` too, or wasm-ld refuses the link
   (`core/README.md`).
4. **Global names in C dependencies.** libjuice, which libdatachannel bundles
   for ICE, exports a global `udp_sendto` (`src/udp.c:204` in libjuice 1.7.2,
   the version libdatachannel v0.24.5 carries). lwIP exports one too, and
   libzt carries lwIP, so a binary with both ZeroTier and WebRTC fails to link:
   `juice-static.lib(udp.c.obj) : error LNK2005: udp_sendto already defined
   in zt.lib(udp.c.obj)`. DevilutionX hit this on 2026-09-26. It renames
   libjuice's copy in its own FetchContent shim:
   `target_compile_definitions(juice-static PRIVATE udp_sendto=juice_udp_sendto)`.
   The rename is safe because libjuice uses the name only for that function
   and its calls. None of libjuice's other `udp_*` names match lwIP's.
   Renaming it upstream in libjuice would spare every other consumer.
   **The fix chosen (2026-09-26) is a shared libjuice.** A libjuice DLL exports
   only its API: MSYS2's 1.7.3 exports its 32 `juice_*` functions and no
   `udp_*` at all. So its `udp_sendto` never meets lwIP's. snps links an
   installed one whenever `find_package(LibJuice)` finds it, and falls back to
   the bundled static copy otherwise (`cmake/LibDataChannel.cmake`);
   `USE_SYSTEM_JUICE=OFF` forces the bundled one.
   Verified 2026-10-03 under MSYS2 UCRT64 with GCC 16.2 and that package:
   - the core and the harness built against it, and the harness passed;
   - a program defining a `udp_sendto` of its own, as lwIP does, linked beside
     the core and ran;
   - against the bundled static copy, the same program did not link:
     `multiple definition of 'udp_sendto'`, from libjuice's `udp.c:204`.

   An MSVC build does not look for one on `PATH`, where an MSYS2 `bin`
   directory would offer a MinGW build. An MSVC libjuice, such as vcpkg's, is
   found the usual way; that is untested. `webbed.snp` always takes the bundled
   copy, since it has to be one self-contained DLL.
