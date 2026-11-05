# The server side

Yes, this needs one. But "a WebRTC server" is four separable things with very
different costs, and only two of them are actually WebRTC's doing.

## 1. Signalling — mandatory, unavoidable, small

WebRTC deliberately does not specify how peers exchange an SDP offer/answer and
ICE candidates. Before any connection exists, something has to carry a few
kilobytes of text between two parties who cannot yet reach each other. There is
no clever way around this; it is inherent to the model.

Use WebSocket. It works in the browser, and `libdatachannel` and
`datachannel-wasm` mirror each other's WebSocket API, so the signalling client
is write-once across native and wasm. This is the concrete reason to leave
`NO_WEBSOCKET=OFF` when trimming dependencies.

Where a host will not upgrade a connection — shared hosting, often — the same
server speaks the same protocol over plain HTTP: Server-Sent Events for what
it pushes, POSTs for what clients send, long polling when a proxy buffers
(`signalling-protocol.md`, "Over HTTP"). Clients find out by themselves when
given an `https://` url. That client is not write-once: the browser's is
`fetch()`, Windows' is WinHTTP, and other platforms need one of their own
(`core/README.md`).

## 2. STUN — effectively mandatory, nearly free

Stateless, tiny, tells a peer its own public address so ICE can work. Public
servers exist; running one is trivial. Not a burden and not a decision.

## 3. TURN — optional, and the expensive one

Relays all traffic when ICE finds no direct path. Bandwidth is the dominant line
item in any WebRTC infrastructure bill, because everything flows through the
server in both directions. How often it is needed depends entirely on the user
base — the honest range quoted by people who measure it is anywhere from 0 to
50% of sessions.

**Our traffic profile is unusually favourable.** Storm caps messages at roughly
500 bytes and runs 8 turns per second; a relayed player costs single-digit KB/s,
not a video stream. Relay is affordable here in a way it simply is not for
conferencing.

Do not write one — `coturn` or `eturnal`. Credentials should be minted by the
signalling server as short-lived tokens (the usual TURN REST pattern) rather
than compiled into the client, so a leaked build does not become someone else's
free relay.

## 4. The lobby — ours, not WebRTC's

The one people forget. Storm's `LockGameList` expects a list of advertised
games; DevilutionX's `base_protocol` discovers peers with `send_oob_mc`
multicast. Both are LAN mechanisms. Over the internet something has to hold
"who is hosting right now", and that is a consequence of replacing LAN
broadcast, not of choosing WebRTC. It is also exactly the custom-server workflow
this project exists to enable.

Signalling and the lobby merge naturally — both answer "who is out there and how
do I reach them" — so realistically this is **one service we write, plus coturn**.

## What storm forces on the data model

Before any message design: `docs/snp-abi.md` §6 establishes that storm gossips
peer addresses between players as opaque 16-byte blobs and then asks the module
to send to peers it has never met. That is not a detail — it determines what the
server fundamentally *is*.

- **A peer id is 128 bits, minted by the server**, stable for the session and
  identical for every participant. It fits `SNETADDR` exactly, and DevilutionX's
  `endpoint::addr` is the same sixteen bytes, so one identity serves both hosts.
  An IP and port cannot serve this role off a LAN.
- **The server is the introduction service for every pair in the room**, not
  just host-to-joiner. Any peer may need to reach any other peer at any moment,
  discovered through gossip we never see.
- **Peers stay attached for the whole session.** The signalling connection is
  not a join-time handshake that closes; it is open for the duration, because a
  new introduction can be needed at any point.
- **Nothing the server holds is precious.** Each client owns its part and hands
  it back after a reconnect: its id, as a resume token the server signed; its
  advert; the game it joined. So the server can be restarted or replaced
  mid-game, and only a secret has to pass from one instance to the next
  (`signalling-protocol.md`, "Reconnecting").
- **But the server does not orchestrate the mesh.** Storm's own gossip drives
  it: a joiner is announced, every peer's storm sends to them, every module
  connects on demand. There is no topology to manage.

DevilutionX adds one requirement storm does not: `send_oob_mc` is a *multicast*
primitive used for discovery, and there is no WebRTC equivalent. The server has
to offer a **room broadcast** over the signalling channel to stand in for it.

## Decisions worth making before writing protocol

### How a player finds a game — DECIDED: a directory

Two shapes, and they change what the server is. **We are building the
directory.** The alternative is recorded below because it constrains what the
directory has to get right.

**A directory.** The lobby holds a public list of open games; a client asks
"what is running?" and gets rows back. This is what LAN broadcast produced and
what storm's game-list screen renders, so it is the faithful reproduction. It
also makes us a public game service: name filtering, abuse, squatting,
moderation, and a list that is embarrassing when empty.

**A join code.** Nothing is published. The host creates a game, the server
returns a short code, the host passes it to whoever they want by whatever means,
and joining means entering that code. The server only maps code → host. Nearly
stateless, private by default, no moderation surface.

Storm's UI shows a *list*, but that is not an obstacle: the list can contain
exactly the one game a code resolves to. DirectIP already sets the precedent for
asking the player something storm has no field for — it raises its own modal
dialog from `initialize()` to collect an IP and port before the game list is
ever drawn.

The two are not exclusive, and the directory does not preclude adding codes for
private games later. But choosing it means the lobby stores public state, so the
things the join-code shape dodged are now ours to handle: room lifetime and
cleanup of dead entries, bounded and sanitised game names (storm's `gamename` is
`char[128]` — see the ABI note below), scoping by product and version, and some
answer to abuse and squatting before the server is public.

### Mesh, star, or something between

The instinct that there is a middle ground is right, but the specific target —
*fewer connections than mesh, at most two hops* — is already the star, exactly.
Any connected graph on N nodes needs at least N−1 edges, a spanning tree has
exactly N−1, and **the star is the only spanning tree with diameter 2**. A chain
has the same edge count and a diameter of N−1. So there is nothing between star
and mesh on that particular axis.

The axis where a middle ground genuinely exists is **degree**, not hops. Keep
diameter 2, but bound how many connections any one node carries, so there is no
hub. The Moore bound says a diameter-2 graph with maximum degree d holds at most
d²+1 nodes, so eight players at degree 3 is comfortably feasible — the Wagner
graph (an 8-cycle with its four diagonals) is 3-regular, 12 edges against a
mesh's 28, and every pair is within two hops with no node carrying more than
three connections.

**And yet mesh is still probably right here, for a reason specific to these
games.** Storm is lockstep: turns advance at the pace of the slowest peer. What
matters is therefore the *worst* path in the session, not the average, and mesh
is what minimises the worst path. One two-hop pair anywhere degrades the whole
game for everyone. `SNETCAPS.latencyms` being a single static number (see below)
makes this worse — it has to cover the worst path too.

The rest of the cost is ours to write: a forwarding layer, loop avoidance,
topology repair when a relaying peer drops, and a much harder problem for
DevilutionX's *reliable ordered* channel, where forwarding means terminating and
re-originating the stream. Storm's player-drop model is not built for a network
that reshuffles mid-game. At four to eight players — six to twenty-eight
connections — none of that complexity pays for itself.

#### Would an indirect path ever actually be faster?

Yes, and it is well measured — the assumption that direct is always best is
simply wrong.

MIT's Resilient Overlay Networks work (SOSP 2001) instrumented 12–16 nodes over
several days and found that routing through **a single intermediate node** beat
the direct Internet path often enough to matter: 11% of samples improved by
40 ms or more in the first deployment, 8.2% in the second, and around 5% saw
loss probability cut by roughly 0.05. Their conclusion also validates the
two-hop instinct directly — "forwarding packets via at most one intermediate RON
node is sufficient".

Valve says the same thing about games, in production, at scale. Steam Datagram
Relay's documentation: "for a surprisingly high number of players, we can also
find a faster route through our network, which actually improves player ping
times" — and, answering the other question at the same time, "IP addresses are
never revealed".

The reason is that BGP chooses paths by policy and AS hops, not latency. Two
ISPs that peer badly can trombone through a distant exchange, while a
well-connected third party is closer to both of them than they are to each
other. For a lockstep game, loss matters as much as round-trip time: a path with
2% loss stalls turns regardless of its ping.

**But note what the intermediate node is in both cases: a machine in a
datacentre with good peering.** Another player's home connection is not that.
Consumer uplinks are asymmetric, bufferbloated and variable, so relaying through
a peer is unlikely to beat a direct path on performance. The two ideas separate
cleanly:

- **Peer relay** — a *connectivity* fallback where ICE fails. Saves TURN
  bandwidth. Not a latency play.
- **Server relay** — the actual overlay-acceleration play, and the one that
  could genuinely beat direct. Steam Datagram Relay, scaled down.

#### Is there a library for it?

Effectively no, and ICE works against it. Candidate pairs are prioritised by
*type* — host, then server-reflexive, then relay — so ICE picks the direct path
even when a relayed one measures faster. It optimises for directness, not for
latency. Nothing in libdatachannel or libjuice will do this for you.

What exists elsewhere: Valve's **GameNetworkingSockets** is the closest prior
art, but the routing half is Valve's operated network and Steam-only. **libp2p**
has circuit relay and DCUtR, but relay there is connectivity fallback, not
latency-routed. **Tailscale** picks the lowest-latency DERP *region* without
comparing relay against direct. **cjdns** and **Yggdrasil** genuinely route
through peers on metrics, but they are general-purpose mesh overlays, heavy, and
not built for lockstep traffic.

So this would be ours to build, which is an argument for deferring it — and for
keeping the option cheap.

#### What to do now

Mesh, direct paths, and **measure**. Per-peer RTT is needed regardless:
DevilutionX's protocol concept requires `get_latency_to`. Two cheap things
preserve the option without building a routing layer:

1. **Put a destination peer id in the wire header from the first commit**, so a
   packet can be forwarded later without a format change.
2. **Record measured RTT and loss per path**, so the decision is eventually made
   against this player population rather than against RON's 2001 numbers.

One storm-specific consequence, because it inverts the usual framing: `latencyms`
is static and must cover the worst path in scope, so supporting relayed sessions
normally forces a *looser* number and slower turn pacing for everyone. But if
server relay genuinely reduces the *worst case* — which is what SDR claims — then
forcing relay could justify a *tighter* `latencyms`. For lockstep, predictability
is worth more than the mean.

**The version of the peer-relay idea that pays today: peer relay as the TURN
fallback.**
Keep mesh as the intent, and when ICE fails for one specific pair, route that
pair through a peer that can reach both instead of allocating TURN. A TURN path
is *already* two hops, so this costs no additional latency over the alternative
it replaces, and it moves that bandwidth off our server onto a player's uplink.
No global topology, no routing protocol — one per-pair decision, made only where
a direct path did not happen.

Two caveats. It burdens a player's connection, so prefer the host or the
best-connected peer. And DTLS is hop-by-hop: the relaying peer can read what it
forwards unless we add end-to-end encryption above the transport. That may not
matter for game turns, but it should be a decision rather than an oversight.

### Peers see each other's addresses

Inherent to peer-to-peer — ICE candidates *are* addresses — but not unfixable,
and worth ordering the options by what they cost:

- **Force relay** (`TransportPolicy::Relay`). Peers see only the TURN server.
  Complete, simple, and costs full relay bandwidth for every session.
- **Relay by default, direct on mutual consent.** Policy is per peer connection,
  so this is expressible; the cost is negotiating it in the signalling protocol.
- **Direct, with disclosure.** Roughly the status quo: DirectIP requires you to
  know the host's IP before you can play at all.
- **Accounts and a friends list.** Heaviest by far — identity, persistence,
  recovery, moderation. Worth noting that a join code gets most of the same
  benefit for none of that: if the only people in your game are people you handed
  a code to, "peers can see each other" is a much smaller statement.

One browser-specific wrinkle for the wasm build: browsers replace host
candidates with mDNS `.local` names, which hides *LAN* addresses but not the
server-reflexive public one. It is not a privacy mechanism, just a smaller leak.

The bar to clear is not perfection, it is the status quo — and the status quo is
typing someone's IP address into a dialog box.

## The ABI constraint on all of this

`SNETCAPS.latencyms` is fixed at **enumeration** time — before any peer exists,
before any path is negotiated — and storm derives all five of its timeouts from
it (`SNET.CPP:645`):

```c
timetoackturn = max( 250,  5 * latencyms);
timetoblock   = max(5000, 12 * latencyms);
timetogiveup  = max(1000,  4 * latencyms);
timetorequest = max(  25,      latencyms / 2);
timetoresend  = max(  50,      latencyms);
```

We cannot adapt it per connection. So the value has to tolerate the worst path
we intend to support: DirectIP ships 50 ms, which suits a LAN, while a TURN hop
across an ocean is 150–300 ms. **If relayed sessions are in scope, `latencyms`
must be chosen for a relayed round trip**, or those sessions will time out
inside storm before our transport ever reports a problem.

This couples a server-topology decision to a constant baked into a struct at
startup, which is not obvious and is expensive to discover late.
