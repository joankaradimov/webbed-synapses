# The DevilutionX side

DevilutionX is the Diablo-side target. It does **not** load `.snp` modules —
there is no `SnpQuery`, `SnpBind` or `SNETSPI` anywhere in its source. It keeps
Blizzard's `SNet*` façade (`Source/storm/storm_net.cpp`) and reimplements
everything beneath it in `Source/dvlnet/`.

Read against `diasurgical/devilutionX` at 1.6.0-dev. It is C++23 and builds x64
on Windows, so nothing here shares a binary with the 32-bit SNP module.

**Where it stands** (2026-09-26). `protocol_webrtc` is written in DevilutionX's
own tree, not here; this repository supplies `snps::core` and the signalling
server. DevilutionX's peers set `Options::open` and use no rooms. `P` maps onto
the core like this:

| `P` | `snps::Transport` |
|---|---|
| `send` | `send` on `Channel::Reliable` |
| `send_oob` | `sendViaServer` (it only ever carries `PT_INFO_REPLY`) |
| `send_oob_mc` | `multicast` |
| `recv` | `receive`, on any channel |
| `disconnect` | `disconnect` |
| `get_disconnected` | `lost` |

The product is `DVLX`, version 1. It plays on x64 desktop, and between a browser
tab and a desktop game, either one hosting (2026-09-27). The web build's wasm
grew by about 0.75 MB, from 6.35 MB to 7.1 MB. The rest of this page is the
analysis that led there. Since it was written, `P` has also gained
`disconnect(endpoint)`.

## Where a transport plugs in

```
SNet* façade            Source/storm/storm_net.cpp
  abstract_net          the interface the game talks to
    base                sequencing, turns, players    Source/dvlnet/base.cpp
      base_protocol<P>  template over a transport     Source/dvlnet/base_protocol.h
        P = protocol_zt ZeroTier today                Source/dvlnet/protocol_zt.h
```

`tcp_client` implements `abstract_net` directly; ZeroTier goes through
`base_protocol<P>`. **`P` is the seam we want** — it is the narrowest surface
and the one that already carries the concepts a peer-to-peer transport has.

Registration is one case in `abstract_net::MakeNet`:

```cpp
case SELCONN_WEBRTC:
    return std::make_unique<cdwrap>([]() {
        return std::make_unique<base_protocol<protocol_webrtc>>();
    });
```

plus a `SELCONN_*` id, an entry in `ConnectionNames`, and a list item in
`Source/DiabloUI/multi/selconn.cpp`. Note that `selgame.cpp` branches on
`SELCONN_ZT` in about eight places for internet-vs-LAN wording and behaviour; a
new internet-capable provider will need the same treatment.

## What `P` must provide

Taken from `protocol_zt`:

```cpp
class endpoint {
    std::array<unsigned char, 16> addr;
    explicit operator bool() const;                 // non-zero == valid
    bool operator==/!=/<(const endpoint&) const;
    buffer_t serialize() const;                     // exactly 16 bytes
    std::expected<void, PacketError> unserialize(const buffer_t&);
    void from_string(const std::string&);
};
struct EndpointHash { uint64_t operator()(const endpoint&) const noexcept; };

std::expected<void, PacketError> send(const endpoint&, const buffer_t&);
bool send_oob(const endpoint&, const buffer_t&) const;
bool send_oob_mc(const buffer_t&) const;
bool recv(endpoint&, buffer_t&);
bool get_disconnected(endpoint&);
std::expected<bool, PacketError> network_online();
std::expected<bool, PacketError> peers_ready();
bool is_peer_connected(endpoint&);
std::optional<bool> is_peer_relayed(const endpoint&) const;
std::optional<int>  get_latency_to(const endpoint&) const;
static std::string make_default_gamename();
```

Three things to notice.

**The endpoint is 16 opaque bytes** — exactly the width of storm's `SNETADDR`.
One peer identity can pass through both adapters unchanged.

**`send` and `send_oob` have different reliability.** In `protocol_zt`, `send`
frames the payload and queues it on a TCP connection: reliable and ordered.
`send_oob` goes over UDP, and `send_oob_mc` sends to a fixed multicast address —
that is how game discovery works. So this host wants a reliable ordered channel
per peer *plus* an unreliable broadcast, which is the opposite of what storm
wants from an SNP. The WebRTC design has to serve both.

**`is_peer_relayed` and `get_latency_to` map onto ICE almost exactly** — relayed
is "the selected candidate pair is a relay candidate", latency is the pair's
RTT. The concept fits a WebRTC transport better than it fits ZeroTier.

## Consequences for our core

- The transport core cannot be C++23: the SNP side is 32-bit MSVC. Keep the core
  behind a narrow C API and put the `std::expected`-flavoured wrapping in the
  DevilutionX adapter.
- Two data channels per peer connection — reliable/ordered and
  unreliable/unordered — so each host gets the semantics it assumes.
- Discovery differs in shape: `send_oob_mc` is a broadcast primitive, while the
  SNP side maintains an explicit game list. A signalling server can serve both,
  but the adapters will not share that code.
- Upstreaming a new `SELCONN_*` means agreeing with DevilutionX maintainers on a
  signalling dependency. Worth raising early; a fork is the fallback.

## How DevilutionX takes a network dependency

Worth knowing before proposing one, because the pattern is narrow and
established.

`CMake/Dependencies.cmake`:

```cmake
if(NOT NONET AND NOT DISABLE_TCP)
  add_subdirectory(3rdParty/asio)
endif()

if(NOT NONET AND NOT DISABLE_ZERO_TIER)
  add_subdirectory(3rdParty/libzt)
endif()
```

and `3rdParty/libzt/CMakeLists.txt` is a FetchContent shim pointing at
**`diasurgical/libzt`** — a fork they control — pinned to an exact commit and
linked as `zt-static`. So a static library, fetched and built in-tree, *is* how
this project takes a network stack today. The shape Joan proposed is the shape
upstream already uses.

Three constraints that follow:

**The dependency budget is small.** `vcpkg.json` asks for `bzip2`, `lua`,
`magic-enum`, plus SDL; `libsodium` only under an opt-in `encryption` feature.
No OpenSSL, no curl. A DTLS/ICE/SCTP stack is the largest thing anyone would
have added, and they will likely want it behind a fork they control. Keep the
core's dependency set small and its crypto backend swappable — that is an
argument for a compact library over Google's libwebrtc that has nothing to do
with binary size.

**Exceptions and RTTI are not guaranteed.**

```cmake
cmake_dependent_option(DEVILUTIONX_DISABLE_RTTI "Disable RTTI" ON "NONET" OFF)
cmake_dependent_option(DEVILUTIONX_DISABLE_EXCEPTIONS "Disable exceptions" ON "DISABLE_ZERO_TIER" OFF)
```

Exceptions are enabled *because ZeroTier needs them*. A core that needs them
lands in the same bucket, which is precedented and therefore acceptable; a core
that does not is strictly more portable and keeps the constrained platforms
open.

**Everything must be gateable.** Honour `NONET`, and add a `DISABLE_WEBRTC` in
the same style. DevilutionX builds for Switch, Vita, 3DS and Android; a
transport that cannot be compiled out is not upstreamable.

## ZeroTier is not WebRTC, and what that implies

Worth stating plainly, because the two solve overlapping problems and it is easy
to assume `libzt` already carries a WebRTC stack. It does not.

ZeroTier's own protocol documentation is explicit: it uses none of WebRTC, ICE,
STUN/TURN, DTLS or SCTP. VL1 packets are encrypted with Salsa20 and
authenticated with Poly1305, asymmetric crypto is Curve25519/Ed25519, peers find
each other through roots ("planets and moons"), attempt hole punching, and fall
back to relaying through those roots when a direct path cannot be established.
`libzt`'s externals are `ZeroTierOne`, `concurrentqueue`, `lwip` and
`lwip-contrib` — no ICE, no DTLS, no SCTP anywhere.

So WebRTC is not a repackaging of what is already there. It is a second stack
solving the same problem differently, and a proposal to add one has to say what
it buys. Three things:

**The browser.** `CMake/platforms/emscripten.cmake` sets

```cmake
set(DISABLE_ZERO_TIER ON)
set(DISABLE_TCP ON)
```

DevilutionX has a full Emscripten target — shell page, pre-js, the lot — and
**no multiplayer in it at all**. ZeroTier cannot work there; a browser has no
raw UDP socket. WebRTC data channels are the only peer-to-peer transport a
browser has. This is a capability the project does not currently have, not an
alternative to one it does.

**Infrastructure independence.** ZeroTier's fallback path runs through
ZeroTier's roots. WebRTC needs signalling and STUN/TURN that *we* run — which is
the custom-server workflow this project exists to enable.

**Two reliability modes on one connection**, which is what lets a single
transport serve both storm (unreliable, it retransmits itself) and
`base_protocol` (reliable ordered plus unreliable out-of-band).

The honest counterweight: on desktop, for Diablo, ZeroTier already works. Pitched
upstream as a ZeroTier replacement this goes nowhere and deserves to. Pitched as
*web multiplayer, and a self-hostable path*, it is a straightforward argument.

The precedent also sets the effort budget. What they already accepted for
ZeroTier is zto + lwip + miniupnpc + libnatpmp, statically linked from a fork
they control. A compact WebRTC stack is comparable or smaller. The dependency
ask is not out of line — but expect them to want the same arrangement: their
fork, pinned to a commit.

## A licensing caution

DevilutionX is under the **Sustainable Use License** — source-available,
non-commercial, not OSI open source. This repository is MIT. Two consequences:

- **Do not copy DevilutionX code into this repository.** `protocol_webrtc` must
  be written against their interfaces — the concept above, their headers — not
  derived from `protocol_zt.cpp`. Modelling the structure of an implementation is
  fine; lifting the file is not.
- A file upstreamed into their tree lives under their license there. Keeping our
  own MIT copy is fine because we authored it; that is only true if the first
  point held.

Since the adapter moved into DevilutionX's tree, only the first point still
binds this repository: nothing of theirs comes here. The adapter is theirs,
under their license, and may follow `protocol_zt.cpp` as closely as it likes.
What crosses over is the core, which stays MIT.
