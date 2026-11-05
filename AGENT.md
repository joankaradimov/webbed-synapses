# AGENT.md

Working brief for anyone — human or agent — contributing to webbed-synapses
(`snps` for short, which is what the code uses). Read this file first. Read
[`docs/snp-abi.md`](docs/snp-abi.md) before touching anything `storm.dll`
calls.

## 1. What this is

webbed-synapses builds **SNP modules**: network service providers that
Blizzard's `storm.dll` loads to move game traffic.

A `.snp` is a 32-bit Windows DLL exporting exactly two functions. Storm scans
its own directory for `*.snp`, asks each module what providers it offers, shows
them in the game's network-selection menu, and binds to the one the player
picks. Everything above the wire — player list, turn scheduling, sequencing,
retransmission, the game's netcode — is storm's job. The SNP does **discovery,
addressing, and datagram send/receive**. Nothing else.

The goal is modern transports behind that original interface, so unmodified game
binaries can use them.

**Targets**

1. **StarCraft / Brood War 1.16.1** — a real `.snp` loaded by the retail
   `storm.dll`. Primary.
2. **DevilutionX** — which has no SNP loader at all. It is a different
   integration entirely, not a second version of the same interface. See §4.

## 2. Rules that are not negotiable

These are ABI facts, not style preferences. Breaking one produces a crash or a
silent hang inside a 1998 binary you cannot debug.

1. **32-bit x86 only.** Every table slot and struct field is DWORD- or
   pointer-sized. A 64-bit build is not a build.
2. **`__stdcall` everywhere.** Both exports and every function-table entry.
   One `__cdecl` slot corrupts the stack on storm's first call.
3. **Storm-facing struct layout is a contract.** Never add, remove, or reorder
   a field in anything storm reads. Every such struct has offset and size
   `static_assert`s in `tests/abi_layout.cpp` — if you change a struct and the
   asserts still pass, you changed the wrong thing.
4. **Never invent a signature, offset, or constant.** If you cannot cite a
   reference source for it (§7), it is a guess — mark it `UNVERIFIED` in a
   comment and say so in your response. Guesses that look confident are the
   single most expensive failure mode in this project.
5. **A `.snp` is the DLL with `caps.mpq` appended to it.** Storm opens the
   module file *as an MPQ archive* and reads `caps.dat` out of it; a module
   without one is skipped during enumeration and never appears in the menu.
   The archive must also begin on a 512-byte boundary, because that is the
   granularity storm searches at. `add_snp_module` generates the archive and
   `cmake/SnpPackage.cmake` appends it — do not hand-roll either. What goes
   *in* `caps.dat` is a provider record per index `SnpQuery` answers, and
   storm believes it over `SnpQuery`; declare both from one
   `snp_caps_records[]` (see `adapters/snp/null/null_caps.h`).
6. **Storm allocates nothing on the receive path.** `Receive` hands storm two
   interior pointers into one block the module owns; storm later calls `Free`
   with the *address* pointer, and the module must recover the block from it.
   Put the address first in the packet struct or translate explicitly.
7. **Signal the receive event.** Storm blocks on the `HANDLE` passed to
   `Initialize`. Queueing a packet without `SetEvent` stalls the game even
   though `Receive` would return data.
8. **`Send` may target up to 16 addresses in one call.** Storm really does this.
   Sending only to `addrlist[0]` silently drops peers — a bug present in the
   most-copied public implementation.

## 3. Layout

```
README.md                         the project for people who want to play with it
AGENT.md                          this file
docs/snp-abi.md                   storm's interface, field by field, with sources
docs/devilutionx-integration.md   the DevilutionX target and its constraints
docs/webrtc-stack.md              why libdatachannel, and how it is configured
docs/server-side.md               what the server is for, and the decisions in it
docs/signalling-protocol.md       the message set
docs/references.md                where ground truth lives and how to fetch it

core/                             the transport. Knows nothing about either host.
adapters/snp/                     storm's function table -> core. 32-bit MSVC DLL.
  include/snp/storm_spi.h         the ABI -- storm's side of the contract
  null/                           the null provider: proof the ABI is right
  webrtc/                         the real provider, webbed.snp: "WebRTC (Internet)"
tests/abi_layout.cpp              ~60 layout assertions; compiled, never linked
tests/harness/                    peers through a real server, no game; snps-echo
tests/wasm/                       the core in WebAssembly: Node, and a browser
tests/snp-sim/                    storm, played twice against webbed.snp
server/                           signalling and the game directory (Node, ws)
cmake/SnpModule.cmake             add_snp_module(): DLL + .def + caps.mpq -> .snp
cmake/SnpPackage.cmake            the concatenation itself, in script mode
cmake/StormLib.cmake              StormLib, fetched for the caps builder only
cmake/LibDataChannel.cmake        libdatachannel, Mbed TLS, nlohmann/json
tools/                            developer scripts and build tools
tools/caps-gen/                   a module's caps.dat, from its own declaration
tools/mpq-builder/                directory -> MPQ (snp-mpq), and back out again
tools/inspect-snp.mjs             reads a .snp the way storm does, without StormLib
tools/load-snp/                   loads a module and runs storm's enumeration
tools/signal-smoke.mjs            the signalling protocol against a running server
tools/webrtc-probe/               32-bit libdatachannel build experiment (§9)
```

The dependency direction is one-way: adapters depend on `core`, `core` depends
on neither. If something in `core/` needs to know whether it is talking to storm
or to DevilutionX, it is in the wrong place. New SNP provider → new directory
under `adapters/snp/`. The DevilutionX adapter is not here: it lives in
DevilutionX's own tree and fetches the core (`docs/devilutionx-integration.md`).

## 4. Two targets, one transport core

The two consumers are not two versions of one interface. They are different
integration points, and conflating them cost this project a design detour.

**StarCraft / Brood War 1.16.1** loads a `.snp`: the ABI in `docs/snp-abi.md`,
a 32-bit MSVC DLL, `__stdcall` function table.

**DevilutionX does not load `.snp` files.** There is no `SnpQuery`, no
`SnpBind`, no `SNETSPI` anywhere in its source. It keeps Blizzard's `SNet*`
façade in `Source/storm/storm_net.cpp` and reimplements everything beneath it in
`Source/dvlnet/`. Its plug-in point is a **protocol class**: `base_protocol<P>`,
where `P` is `protocol_zt` today. Adding a transport means writing
`protocol_webrtc` plus a `SELCONN_*` id — a C++23 class compiled into a 64-bit
binary, not a DLL, not an ABI. See `docs/devilutionx-integration.md`.

### What this settles

The Diablo/Hellfire-era SNP table is **not a target**. There is exactly one SNP
era to support, which means:

- slot 11 `Receive` has one contract, `(addr, data, databytes)`
- slot 16 has one arity
- `SNP_STORM_ERA`, runtime era dispatch, indirect-jump thunks, and
  descriptor-size sniffing are all unnecessary. **Do not build them.**

`SNETSPI_DIABLO` stays in the header as the historical record — it is what makes
the StarCraft table's provenance checkable, and the cross-era offset assertions
are what proved slots 1–19 are shared. It is documentation, not a build target.

### What the targets actually share

The transport, not the interface.

| | StarCraft SNP | DevilutionX protocol |
|---|---|---|
| Artifact | 32-bit MSVC DLL, C ABI, `__stdcall` | C++23 class, x64 |
| Peer address | `SNETADDR` — 16 opaque bytes | `endpoint::addr` — 16 opaque bytes |
| Reliability the host wants | **unreliable, unordered** — storm does its own sequencing, acks and retransmission | **reliable, ordered per peer** (`send`), plus unreliable out-of-band (`send_oob`) |
| Discovery | game list via `LockGameList` | `send_oob_mc` to a fixed multicast endpoint |

The endpoint width matching is a gift: one 16-byte opaque peer identity passes
through both adapters unchanged.

The reliability split is the real constraint on the WebRTC design. One peer
connection carrying **two** data channels — reliable/ordered and
unreliable/unordered — with each adapter using the one its host expects.
Layering reliability under storm would wreck the RTT model that drives all five
of its timeouts; withholding it from DevilutionX would break `base_protocol`'s
framing assumptions.

The core has to compile in both worlds — 32-bit MSVC on one side, 64-bit C++23
on the other — which argues for a narrow C API and no dependency on either
host's language level.

## 5. Build

CMake, out-of-source, 32-bit. The SNP modules refuse to configure if the
toolchain targets 64-bit; `SNP_BUILD_PROVIDERS=OFF` leaves them out, and the
core and the harness then build for x64 as well (MSVC, 2026-09-26).

MSVC x86 is the primary toolchain; the others exist for CI and for checking the
ABI without Windows.

```
cmake --preset msvc-x86        # primary: Visual Studio, native Windows
cmake --build --preset msvc-x86

cmake --preset mingw-x86       # cross-compile from Linux/WSL
cmake --build --preset mingw-x86

cmake --preset abi-check       # host layout check, no Windows, no linking
cmake --build --preset abi-check
```

`abi-check` compiles `tests/abi_layout.cpp` for a 32-bit target without
linking. The `static_assert`s *are* the test: it compiles or it doesn't, so
layout regressions are caught in CI on any host with a 32-bit-capable
compiler. The shim it uses is freestanding, so no 32-bit libc is needed.

**The transport fetches its stack at configure time**: libdatachannel,
Mbed TLS and nlohmann/json, from `cmake/LibDataChannel.cmake`, before any of
our compile options are set so `/W4 /permissive-` stays out of them.
`SNPS_BUILD_TRANSPORT=OFF` skips all of it, as `abi-check` does. Everything
links the static CRT when this is the top-level project. A project that takes
only the core — DevilutionX does, with `SNP_BUILD_PROVIDERS=OFF` and
`SNP_BUILD_TESTS=OFF` — keeps its own runtime. `ctest` runs the harness and
`snp-sim`, each against a signalling server it starts itself. The server and
the test drivers are all Node, with `npm ci` done in `server/`; nothing in the
repository uses Python.

**The browser build** is the same tree under Emscripten:
`emcmake cmake -S . -B build/wasm -G Ninja`. There the core is built on
datachannel-wasm, the browser's own WebRTC, instead of libdatachannel.
`cmake/LibDataChannel.cmake` patches its JavaScript as it is fetched. The
core catches exceptions with JavaScript exceptions, the kind that works with
`-sASYNCIFY`, and hands `-fexceptions` on to whatever links it. `ctest` there
runs `tests/wasm` in Node, and the browser half of that test is run by hand
(`tests/wasm/README.md`, which also says where Emscripten lives on this
machine).

**The caps archive is generated.** A module that names a `CAPS` header gets its
`caps.dat` written from that declaration and packed into an MPQ, which
`SnpPackage.cmake` then appends. This costs a `FetchContent` of StormLib (MIT)
at configure time, and the tools have to run on the build machine — a 32-bit
tool on 64-bit Windows is fine, cross-compiling from Linux is not. Two knobs:

- `SNP_BUILD_CAPS_TOOLS` — off when cross-compiling; turn it off to skip the
  StormLib fetch entirely.
- `SNP_CAPS_SIGN` — on by default: the game accepts Blizzard's weak signature,
  which StormLib can produce, and skips a module whose archive is unsigned
  (`docs/snp-abi.md` §2). Leave it on.
- `SNP_CAPS_ARCHIVE` — a prebuilt archive to use instead. Required if the
  tools are unavailable, or the module builds without one and storm will
  silently refuse to enumerate it. `add_snp_module` warns when that happens.

`tools/inspect-snp.mjs` answers "would storm accept this?" for a built module.

## 6. How to work here

- **Cite your source in the code.** Anything derived from a reference gets a
  comment naming the file and line: `// STORM.H:1472 (Hellfire)`. This is how
  the next agent knows what is verified and what is inherited guesswork.
- **Preserve the confidence markers** in `docs/snp-abi.md`. If you verify
  something previously marked unverified, upgrade it *and* say what you checked
  it against.
- **Layout changes come with assert changes.** Same commit.
- **Keep other projects out of this tree.** The reference sources and the
  games' own files live elsewhere; `docs/references.md` says where the sources
  are. They are other people's work, with their own licensing and provenance;
  this repository is MIT and stays clean.
- **Testing against a real game is the only proof that counts.** The ABI
  compiles fine when it is wrong. "It builds" is not a result; "the game lists
  the provider and joins a match" is.
- When a design choice has more than one defensible answer, surface it rather
  than committing to one quietly.

## 7. Ground truth

Ranked by authority. `docs/references.md` has the detail.

1. **Leaked Hellfire Storm source** — Blizzard's own `STORM.H`, `SNET.CPP`
   (storm's side of the interface), and the `STANDARD` / `BATTLE` provider
   modules. Authoritative for the Diablo era and for *how storm behaves*, which
   is otherwise unobservable.
2. **BWAPI `SNP_DirectIP`** — the only complete public `.snp` for 1.16.1.
   Authoritative for the StarCraft-era table and the build recipe.
3. **devilution `storm.h`** — Blizzard's own symbol names for the Diablo structs.
4. **Skywing's writing** (valhallalegends, blizzhackers, via web.archive.org) —
   protocol and storm internals context.

Where sources conflict, working code beats header comments, and Blizzard's
source beats reverse-engineering.

## 8. Status

Foundation, plus two of the three unknowns retired. In StarCraft, `webbed.snp`
has listed, joined and started a match, but no match has been played through
yet. DevilutionX has played games over the core, on x64 desktop.

- [x] ABI documented and encoded in `adapters/snp/include/snp/storm_spi.h`
- [x] CMake build producing a real `.snp` (DLL + generated `caps.mpq`) — built by
      MSVC x86, and read back by two independent readers
- [x] Layout assertions
- [x] Null provider listed in a real game's network menu — StarCraft 1.16.1,
      2026-09-16, as "Null (no network)"
- [x] Null provider selected and bound in a real game — `SnpBind`, `Initialize`,
      a game created, started and left, `Destroy`; no crash (2026-09-16)
- [x] Era question closed — DevilutionX has no SNP loader, so there is one era (§4)
- [x] Signalling and directory server, in Node — 49 unit tests, plus 20 checks
      over a real WebSocket and a restart between two servers (`npm test` in
      `server/`). It can be restarted or replaced mid-game.
- [x] WebRTC stack proven for 32-bit MSVC — builds and connects
      (`tools/webrtc-probe/`, `docs/webrtc-stack.md`)
- [x] WebRTC transport core — `core/`, with `tests/harness` (56 checks, x86 and
      x64) and `tests/wasm` (wasm: 11 in Node; 13 in a browser, against a
      native peer). Each includes the server dying and being replaced.
- [x] SNP adapter over that core — `adapters/snp/webrtc`, with `tests/snp-sim`
      (29 checks, two copies of the real DLL in one process)
- [ ] Two players in a real game over `webbed.snp` — listed, joined and
      started between two instances of a modified StarCraft build
      (2026-09-24); the match then desynced in that build's game code, not
      the transport (§9)
- [x] `protocol_webrtc` for DevilutionX — in DevilutionX's own tree, over this
      repository's core and server. Up to three instances on MSVC x64
      (2026-09-26, reported by the DevilutionX session): create and join, a
      third player, a player killed, a leave, and host migration
- [x] DevilutionX in the browser — a browser tab and a native x64 instance
      play each other, either one hosting (2026-09-27, reported by the
      DevilutionX session), over the core built on `datachannel-wasm`

**Decided:** MSVC x86 is the primary toolchain for the SNP side. The first real
transport is WebRTC. DevilutionX is the Diablo-side target, via its protocol
interface rather than an `.snp`.

**Settled:** `caps.mpq` generation, which was deferred. The build writes
`caps.dat` from the module's own provider declaration and packs it with
StormLib (`tools/caps-gen`, `tools/mpq-builder`), so a `.snp` is complete
without shipping anyone else's archive. 1.16.1 also demands a *signature*,
which the build provides: an unsigned archive is skipped (`docs/snp-abi.md`
§2, read from the binary on 2026-09-25).

## 9. Where to pick up

**What has been verified, and how.**

- The layout assertions compile for 32-bit and fail correctly when a struct is
  perturbed.
- The primary toolchain works: `cmake --preset msvc-x86` configures and builds
  under Visual Studio 17 2022 with MSVC 14.44, producing a 32-bit `null.dll`
  (`14C machine (x86)`) that exports `SnpQuery` and `SnpBind` undecorated, from
  `__stdcall` thunks of the right arity, and a `null.snp` beside it. The
  packaging is byte-for-byte the module followed by the archive, padded to a
  512-byte boundary when the module is not already aligned.
- The server's 33 unit tests pass under Node 24 with `ws` 8.22. `npm test` also
  drives the whole protocol over a real socket, with Node's own WebSocket as the
  client, so neither end is the other's library. That is 18 checks: welcome,
  advertise, list, product scoping, join, verbatim signal relay, broadcast,
  multicast and relay, the error paths, and host-departure closing the game.
  `tools/signal-smoke.mjs` runs the same checks against a server that is
  already running.
- The WebRTC stack builds and *runs* as 32-bit x86, first as an executable
  (`tools/webrtc-probe/`, `docs/webrtc-stack.md`), now inside the module that
  storm loads (`tests/snp-sim`).
- `null.snp` is now a complete module: 26112 bytes of DLL followed by a
  312-byte generated archive at a 512-aligned offset. Two independent readers
  find `caps.dat` inside it — `tools/inspect-snp.mjs`, which reimplements
  storm's path, and StormLib via `snp-mpq --extract` — and it parses to one
  valid provider record. The `caps.dat` record format was confirmed against
  both Blizzard's `STANDARD/CAPS.DAT` and the one inside BWAPI's shipped
  1.16.1 module.

**What has not.** No match has been played through over `webbed.snp` in a real
game, and nothing has run over anything but loopback. Nothing has been relayed through TURN. The
`mingw-x86` and `abi-check` presets have not been run on this machine, and the
core has not been built for x64 or wasm.

**The first attempt failed.** `null.snp` was dropped into a real 1.16.1
directory and no entry appeared. Two causes were found by reading rather than
by debugging, and both are fixed:

- The caps may have been too modest. Storm lists a provider only if it passes
  `SpiMeetsMinimumCaps` against the caps the *game* asks for
  (`docs/snp-abi.md` §2) — `maxplayers` 1 would fail a game wanting two. The
  null provider now declares BWAPI DirectIP's numbers verbatim. (The other
  suspect, missing `PAGELOCKEDBUFFERS`, was later ruled out: 1.16.1's own
  Battle.net module declares flags 0.)
- The module imported the dynamic CRT, so a machine without the x86 redist
  would fail `LoadLibrary` — silently, like everything else. Modules now link
  the static CRT and `null.dll` imports `KERNEL32.dll` alone.

**The second attempt produced no log at all**, which is weaker evidence than it
looks: an unwritable game directory looks exactly like a module that was never
loaded. The trace now falls back to `%TEMP%` and also goes to
`OutputDebugString`.

**The third attempt worked** (2026-09-16), on `D:\Games\Starcraft-1.16` —
`StarCraft.exe` and `storm.dll` both file version 1.16.1, and a different
executable from the two runs that failed, which confounds what those failures
mean. "Null (no network)" appeared. The breadcrumbs show storm accepting
weak-signed archives and loading each module twice, once to enumerate and once
to bind, exactly as `SNET.CPP` describes; the build now signs by default. The
same run listed "Direct IP" twice — BWAPI's real DirectIP (`SNP.snp`) and our
DLL carrying BWAPI's archive — which settles that 1.16.1 takes its menu
strings from `caps.dat`.

**The fourth attempt bound the provider**: the log reads `SnpBind index 0`,
`Initialize`, `Destroy`, with a game created, started and left in between and
no crash. Blizzard's own `battle.snp` and `standard.snp` in that install give
the first hard bounds on 1.16.1's `mincaps` (`docs/snp-abi.md` §2).

The weak signature covers only the archive region, so signing before appending
survives, and StormLib's key verifies Blizzard's own archive — ours is
indistinguishable from theirs. `snp-mpq --verify` reports what a finished
`.snp` carries.

**The transport exists** (2026-09-17). `core/` implements it over
libdatachannel, `tests/harness` drives three transports through a real server,
and `adapters/snp/webrtc` puts storm's table on top. `tests/snp-sim` loads two
copies of the finished `webbed.snp` into one process and plays storm against
them — list, join, send, receive, free, destroy, unload, reload — which also
retires "prove the stack inside a DLL". Writing the provider caught a wrong
`Send` type in `SNETSPI_SC` (`SNETADDRPTR **` for `SNETADDRPTR *`); the stack
layout was never affected, and the header now says so.

**The first two-player attempt** (2026-09-17, two instances on one machine)
listed nothing on the joining side. The server had stripped control characters
from the stat string, and StarCraft's uses CR as a field separator; the joiner's
game dropped the unparseable listing. Names and stat strings are now bytes end
to end, one character per byte on the wire (`docs/signalling-protocol.md`), and
the harness and `snp-sim` both carry StarCraft's real stat string. The same run
confirmed `programid` `'SEXP'` and `versionid` 211 (`docs/snp-abi.md` §7). The
module now logs what it hands storm's game list whenever that changes.

**Storm itself, without the game** (2026-09-24). A console client drove the
real 1.16.1 `storm.dll` the way a game does: two to four processes created,
listed, joined, played lockstep turns and left over `webbed.snp`, and every
one of them saw the same game. So storm lists and joins these games; whatever
hid them from StarCraft's join screen was between storm's list and the
screen. What storm needed from that client is in `docs/snp-abi.md` §6.

**The join screen, explained** (2026-09-24). StarCraft's own stat-string
parser was hiding the games. The join screen drops any game whose stat string
fails it, and the one on record from 2026-09-17 does, because its tileset is
16 (`docs/snp-abi.md` §7). The host in every two-instance run so far has been
a modified StarCraft build, since retail 1.16.1 will not start twice on one
machine, and that build's extended-tileset work in progress advertised
tilesets retail never does. On a map with a classic tileset, the game was
listed, joined and started.
About nine seconds into the match the two sides desynced and dropped each other.
The joiner had loaded Badlands for a Space Platform map: StarCraft's lobby
gives a joiner only the classic tileset, and that build never set its extended
index on the joining side, a bug of that build's own. The module's log shows
traffic flowing both ways with nothing dropped until the split.

**Renamed** (2026-09-26). The project is webbed-synapses, and the WebRTC
module ships as `webbed.snp`, reading `webbed.ini` and writing `webbed.log`.
Before then it was `webrtc.snp`, which is the name on any older log or
install. `snps` stays as the short form in code, options and the server
package; the README says where the name comes from.

**The DevilutionX adapter moves out** (2026-09-26). `protocol_webrtc` is being
written in DevilutionX's own tree, by its own session. This repository
supplies the core and the server, and makes the changes that adapter needs
from them. The design decisions:

- **Discovery goes through the server.** DevilutionX finds games with a query to
  everyone (`send_oob_mc`), answered by whichever player is currently master
  of each game, and uses no rooms, adverts or joins at all.
- **The server gained open peers.** A peer that says `open` in its `hello` can
  be signalled, sent a `relay` and reached by a `multicast` by anyone in its
  scope.
- **The core gained matching calls.** `Options::open`, `sendViaServer()`,
  `multicast()` and `Channel::Server` carry the query and its answers.
- **Links can be dropped and reported.** `disconnect()` and `lost()` give
  `base_protocol` its `disconnect` and `get_disconnected`.
- **An open channel no longer needs the server.** A server restart mid-game
  stops nothing that is already connected.

WebRTC is on by default in DevilutionX's desktop builds, like ZeroTier; its
product code is `DVLX`, version 1. The harness plays that way of meeting,
server loss included, and the core now builds and passes it on x64 as well.

**DevilutionX plays over it** (2026-09-26), by the DevilutionX session's own
report. It ran up to three instances on MSVC x64, with DevilutionX's DLL
runtime, against a local server:
- a game created and joined by its id, the query answered through the server;
- a third player introduced by the host;
- a player killed mid-game, and a normal leave;
- host migration: after the creator left, a newcomer's multicast was
  answered by the new master.

The lobby's public list and password-protected games were not driven.

Linking ZeroTier and WebRTC into one binary clashed on `udp_sendto`, which
libjuice and lwIP both export (`docs/webrtc-stack.md`, risk 4). DevilutionX
renames libjuice's copy in its shim.

**The core in the browser** (2026-09-27). Under Emscripten the core builds on
datachannel-wasm and needed no source changes to compile. Two bugs surfaced
only once it ran:
- datachannel-wasm's JavaScript read heap views that current Emscripten no
  longer exports, so every data-channel send threw. The fetch patches it.
- The core used a zero `steady_clock` time point to mean "never". A browser's
  clock counts from page load, so a connection request made in a page's
  first two seconds never went out.

With both fixed, `tests/wasm` has a page in Chromium talking to a native
libdatachannel peer: discovery through the server, both data channels, and
disconnect and reconnect (`tests/wasm/README.md`).

The same day, DevilutionX played it for real, by its session's report. A
browser tab joined a native x64 host, and a native instance joined a browser
host: found by multicast, answered through the server, then connected
directly, whichever side offered. That build is threaded, and a threaded
consumer has to compile snps with `-pthread` (`core/README.md`). Its wasm grew
from 6.35 MB to 7.1 MB with snps, datachannel-wasm and JavaScript exceptions.

**The server moved to Node** (2026-10-01), on `ws`, replacing the Python one.
The wire protocol, limits and behaviour are unchanged, down to the two
directory bugs under "Loose ends", and so are its tests, ported to `node:test`.
Every test that starts a server passed against it: the harness, `snp-sim`,
the wasm test in Node, and the browser test against a native peer.

The rest of the Python followed the same day, so nothing in the repository
needs it now:
- `tests/harness/run.mjs`, which now lets the server pick its own port and
  reads it back, so there is no window for another process to take it;
- `tests/wasm/browser.mjs`, which serves the page itself;
- `tools/inspect-snp.mjs`, whose output matched the Python version's line for
  line on seven modules and archives, Blizzard's among them.

**The server can restart** (2026-10-01). Before, a server restart cost every
client its peer id and every game its room. Links already open carried on, but
nobody could join, and a lost link stayed lost. Now nothing the server holds
has to survive it (`docs/signalling-protocol.md`, "Reconnecting"):
- **Resume tokens.** `welcome` carries the peer id signed with `--secret`, and
  any server holding the same secret gives that id back to a `hello` that
  presents it. When an id connects twice, the newer connection wins.
- **Room ids derived from the host's id** and a nonce the host picks per game,
  so a re-advertised game comes back under the id its members already have.
- **The core reconnects by itself** once it has been welcomed, with jittered
  backoff, keeping its links. It re-sends its advert and its join, repeating
  the join until the host's room is back. Status is `Connecting` meanwhile.
  `Offline` now means only that the transport gave up: the first connection
  failed, or the server refused it.
- **A grace period.** A connection that drops without a close frame keeps its
  peer's place for 20 seconds, so a blip goes unnoticed by everyone else.
- **SIGTERM closes with 1012**, "service restart", and tells nobody their game
  closed.

Two directory fixes came with it. An expired room now releases its members and
tells them, which was half of the loose end below. A `join` now leaves the
caller's previous room instead of leaving it a member of both. The harness, the
wasm test in Node and the browser test each kill the server mid-game and check
that the transports come back to its replacement as the same peers. Not
covered by any test: a browser whose server vanishes without closing anything.
A browser's WebSocket cannot ping, so it notices only when TCP gives up. The
native core pings.

The same day, DevilutionX rebuilt both its builds against this, by its
session's report, and `protocol_webrtc` needed no changes. A native x64
instance and a browser tab joined each other's games, either side hosting. A
browser host and a native player were in a game when the server was killed
outright. A new server with the same secret was listening nine seconds later.
Both peers resumed their ids within two seconds of it, and neither side's log
shows a player leaving. A third player then found the game by multicast and
joined it. Its browser logged one error on every create and join, which was
older than all this. DevilutionX discards its lobby's transport while that
transport's WebSocket is still connecting, and browsers complain when a socket
is closed before it opens. The datachannel-wasm patch now lets such a socket
open first, and both halves of `tests/wasm` do the same thing to show it.

**A shared libjuice** (2026-10-03). The `udp_sendto` clash above is fixed the
way decided on 2026-09-26. snps links an installed libjuice DLL whenever it
finds one, whose `udp_sendto` stays inside it, and falls back to the bundled
static copy otherwise; `USE_SYSTEM_JUICE=OFF` forces the bundled one
(`cmake/LibDataChannel.cmake`). `webbed.snp` always takes the bundled copy,
since it has to stay one self-contained DLL. Under MSYS2 UCRT64 with GCC 16.2
and MSYS2's libjuice 1.7.3:
- the core and the harness built against it, and the harness passed;
- a program with a `udp_sendto` of its own linked beside the core;
- against the bundled static copy, the same program did not
  (`docs/webrtc-stack.md`, risk 4).

DevilutionX can drop its rename. Until it does, its shim has to skip the
rename where there is no `juice-static` target, which is wherever a libjuice
is installed. Its MSVC build would need an MSVC libjuice, such as vcpkg's,
which is untested. The browser build has no libjuice at all.
That was also snps's first GCC build. It showed that `tests/abi_layout.cpp`,
which accepts only 32-bit x86, was added to every build. It is now added only
for a 32-bit target, so a 64-bit build of all targets works too.

Next, in the order that retires the most risk:

1. **A match played through over `webbed.snp`.** Everything before the match
   now works (above). Put the module and a `webbed.ini` beside `storm.dll`
   (`adapters/snp/webrtc/README.md`), run the signalling server where both can
   reach it, host on one, join from the other. Two instances on one machine
   need a build without retail's single-instance check. Until that build's
   tileset desync is fixed, a Badlands map may avoid it: a joining instance
   that has loaded no map of its own should load Badlands anyway (untested).
   The module's log records what storm actually passes. Check it against
   `docs/snp-abi.md` §7, whose "observed" values so far come from that
   build's game code, not retail's. A retail host would settle them; retail
   started first with the modified build second may run side by side
   (untested).
2. **The Internet path**: a STUN server for NAT traversal, then TURN for the
   players it cannot help, and a harness run with `relayOnly` against it.
3. **A wasm job in CI.** `.github/workflows/test.yml` runs the server's
   tests, the layout check and the MSVC x86 build with its ctest, but nothing
   builds wasm, which is the target most likely to drift: datachannel-wasm
   implements only part of libdatachannel's API (`docs/webrtc-stack.md`, risk
   2). It already broke unnoticed once: the root `package.json`'s
   `"type": "module"` (2026-10-03) made Node load Emscripten's CommonJS output
   as an ES module, and the wasm test could not start until it went
   (2026-10-08). The browser half of `tests/wasm` needs a browser, which no
   ctest run has.
4. **What DevilutionX's adapter may still ask for.** None of these blocks it:
   - Whether a link runs through TURN, and its round-trip time, for
     `is_peer_relayed` and `get_latency_to`.
   - A way to connect to a peer before there is anything to send, so ICE, DTLS
     and SCTP fit inside `base_protocol`'s five seconds between join request
     and acceptance.
   - A look at the signalling rate limit. A player joining a four-player game
     opens three links at once, which is about 20 signals in a second against a
     bucket of 30.

Loose ends, smaller than any of those:

- **The server refuses a new host inside an existing room.** After the desync
  above, each side played on alone. Storm on the joining side, now owner of its
  half, started advertising it. The server refused with `already_joined`
  (`server/src/rooms.js:164`), because that peer was still a member of
  the old host's room. The module re-sent the advert every ten seconds
  (`kAdvertInterval`, `core/src/transport.cpp:59`) and logged a warning each
  time until the game ended. The same happens whenever storm hands a joiner the
  game while the old host is still connected. (A second way in, an expired
  room that kept its members, was fixed on 2026-10-01; above.) Three fixes, not
  yet chosen between:
  - an advert implicitly leaves the caller's old room;
  - storm's own host migration: a room whose host leaves stays open for its
    members and passes to the next one that advertises, and the first fix
    applies when the old host is still there;
  - keep refusing, but say so once and stop retrying.

  The transport core has no hook for any of these yet. It ignores
  `peer_joined` and `peer_left` (`core/src/transport.cpp:781`). On
  `game_closed` it removes the game from its list and stops joining it again
  after reconnects. Nor does the core have a way to leave a game: a joiner
  stays a member of the server's room after storm has left it, until the room
  closes or the joiner disconnects, and it joins that room again after a
  reconnect. DevilutionX is not affected: its peers are open and use no rooms.
- **TURN credentials expire after an hour** (`--turn-ttl`), and a client only
  gets new ones in a `welcome`. A session that runs longer without
  reconnecting cannot use TURN for a link it makes after that. No TURN server
  is in use yet.
- **`broadcast` has no users.** The server's room-scoped `broadcast` was meant
  for DevilutionX, which uses `multicast` instead. It can go.
- **The `list_*` switches in `webbed.ini`** chased what turned out to be the
  game's stat string, and none of them can change what the join screen shows
  (`adapters/snp/webrtc/README.md`). They can go.
- **Naming.** The module's files are all `webbed.*` now, but the ini section is
  still `[snps]` and the override is still `SNPS_SERVER`. Whether those follow
  the module, or stay as the project's short form, is undecided.
- **Slot 20's field name.** `SNETSPI_SC::StartAdvertisingGameShort` in
  `storm_spi.h` is really `GetLocalPlayerName` (`docs/snp-abi.md` §4).
  Renaming it means changing `tests/abi_layout.cpp` in the same commit.
