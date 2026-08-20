# webbed-synapses

WebRTC networking for StarCraft: Brood War 1.16.1, delivered the way the game
already expects it: as `webbed.snp`, a network provider module that the game's
own `storm.dll` loads alongside Blizzard's `battle.snp` and `standard.snp`. The
game is not modified.

The module appears in the multiplayer menu as **WebRTC (Internet)**. A small
signalling server keeps the list of open games and introduces players to each
other; the game traffic itself goes directly between the players.

```
  player A                                    player B
  StarCraft                                   StarCraft
     │                                           │
  storm.dll                                   storm.dll
     │                                           │
  webbed.snp ◄────── game traffic (WebRTC) ────► webbed.snp
     │                                           │
     └───────────► signalling server ◄───────────┘
                (game list, introductions)
```

## Status

Experimental. It works up to the start of a match, and has not yet been played
through one.

- StarCraft lists the provider, and a game has been listed, joined and started
  over it. The one match attempted so far desynced in the game build used for
  the test, not in the transport.
- Everything so far has run on one machine. The Internet path, through STUN
  and TURN, has not been exercised.
- Diablo, through [DevilutionX](https://github.com/diasurgical/devilutionX), is
  the second target: the same transport behind DevilutionX's own network
  interface. The adapter lives in DevilutionX's tree, and DevilutionX has
  played games over this transport on desktop, and between a browser tab and a
  desktop game, either one hosting.

## Trying it

You need Windows, Visual Studio 2022 with the C++ x86 tools, Git, CMake 3.21 or
newer (Visual Studio's own will do), and Node 22 or newer.

**Build.** Configuring fetches libdatachannel, Mbed TLS, nlohmann/json and
StormLib.

```
cmake --preset msvc-x86
cmake --build --preset msvc-x86
```

The module is `build/msvc-x86/adapters/snp/webrtc/RelWithDebInfo/webbed.snp`.
It links everything statically, so there is no runtime to install alongside it.

**Run a signalling server** where every player can reach it:

```
cd server
npm ci
node src/main.js --port 8765
```

On a LAN that is all it needs. Across the Internet, give it a STUN server with
`--stun`, and a TURN server with `--turn` for players behind symmetric NATs
([`server/README.md`](server/README.md)).

**Install.** Put `webbed.snp` in each player's StarCraft folder, beside
`storm.dll`, with a `webbed.ini` next to it naming the server:

```ini
[snps]
server = ws://192.168.1.10:8765
```

Then choose **WebRTC (Internet)** from the multiplayer menu. Retail 1.16.1 will
not start a second copy of itself, so two players means two machines. The
module logs to `webbed.log` in the same folder;
[`adapters/snp/webrtc/README.md`](adapters/snp/webrtc/README.md) has the other
settings and what the log records.

**Test.** With `npm ci` done in `server/`, `ctest` runs:
- the transport harness;
- a simulated storm against two copies of the module.

Each test starts its own signalling server. The server's own tests are
`npm test` in `server/`.

```
ctest --test-dir build/msvc-x86 -C RelWithDebInfo
```

## The name

A synapse is where a signal crosses from one cell to the next, and a network
provider is where storm's traffic crosses onto the wire. So each module is a
synapse — `.snp` for short — and `snps`, the name the code goes by, is the
plural. Every player connects to every other, so together they form a web. And
before Blizzard was Blizzard, it was Silicon & Synapse.

## Contributing

Read [`AGENT.md`](AGENT.md) first: what an SNP is, the ABI rules that cannot be
broken, the layout, the build, and where the ground truth lives. Read
[`docs/snp-abi.md`](docs/snp-abi.md) before changing anything `storm.dll`
calls.

## License

MIT; see [`LICENSE`](LICENSE). The sources this work was checked against are
other people's and are not included here;
[`docs/references.md`](docs/references.md) says where each one lives.

webbed-synapses is not affiliated with or endorsed by Blizzard Entertainment.
StarCraft, Brood War, Diablo and Battle.net are trademarks of Blizzard
Entertainment, Inc.
