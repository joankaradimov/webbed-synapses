# adapters/snp/webrtc — `webbed.snp`

Storm's function table over `snps::core`. It appears in StarCraft's network
menu as **"WebRTC (Internet)"**. The module is named for the project,
webbed-synapses; this directory is named for its transport.

| Storm asks for | It gets |
|---|---|
| the game list | the signalling server's directory for this product and version |
| a player's address | that player's 16-byte peer id |
| `Send` | the unreliable channel, to every address in the list |
| `Receive` / `Free` | a queued datagram, address first so `Free` can find the block |
| `StartAdvertisingGame` | an advert the transport keeps alive until `StopAdvertisingGame` |
| `GetGameInfo` | the listing, and a `join` so the server will relay introductions |

Everything goes over the unreliable channel because storm sequences and
retransmits on its own (AGENT.md §4).

## Setting it up

Put `webbed.snp` beside `storm.dll`, and a `webbed.ini` beside it:

```ini
[snps]
server = ws://127.0.0.1:8765
log = info
```

- `server` — the signalling server. The `SNPS_SERVER` environment variable
  overrides it; with neither, `ws://127.0.0.1:8765`.
- `log` — `off`, `error`, `warning`, `info` (the default) or `debug`. The log is
  `webbed.log` beside the module, or in `%TEMP%` if that directory is not
  writable, and every line also goes to `OutputDebugString`.

Both files take the module's own name, so every file the provider owns is
`webbed.*`; a copy renamed `x.snp` would read `x.ini` and write `x.log`.

Until 2026-09-26 the module was `webrtc.snp`. An install from before then
needs its `webrtc.ini` renamed to `webbed.ini`, and the old module removed:
with both in the folder, expect the provider listed twice, as two modules
carrying one provider record were for Direct IP (AGENT.md §9).

If the server cannot be reached within four seconds, `Initialize` fails with
`ERROR_NETWORK_UNREACHABLE` and the game says so.

Every player needs the same server. For two machines on one network, run it
where both can reach it:

```
node server/src/main.js --host 0.0.0.0 --port 8765
```

On a LAN, peers connect directly with no STUN server. Across the Internet,
give the server `--stun` and, for players behind symmetric NATs, `--turn` —
see `server/README.md`.

## The join screen

StarCraft's join screen shows only games whose stat string its own parser
accepts, and drops the rest without a word. The parser checks the map size,
the player counts, the tileset (it must be below 8), and the host and map
names, each ended by CR. The screen reads nothing else the module hands storm.
`docs/snp-abi.md` §7 has the rules. The module passes the stat string through
byte for byte, so when the log says `game list now holds 1 game(s)` and the
screen shows nothing, look at the stat string on the host's `advertising`
line.

That was the open problem of 2026-09-17: the host's stat string named tileset
16. The `list_*` switches below were written to chase it. Each one moves a
field of the listing toward what BWAPI's Local PC hands the same screen. The
screen reads none of those fields, so the switches change nothing there. They
are still read on every `Initialize`.

| Key | Default | Local PC's value |
|---|---|---|
| `list_first_id` | `1` | `0` — the host's struct overwrites the id |
| `list_extra` | `pass` (storm's `CLIENTDATA`) | effectively `zero` — it copies the wrong bytes |
| `list_category` | `0xA7` (DirectIP's constant) | `0xA7`; the host passed `0x4000F` |
| `list_latency` | `0x50` | `0x50` |
| `list_show_own` | `0` | `1` — it lists the host's own game |
| `list_owner` | `peer` | a slot number; `zero` breaks joining |

The `listing:` line at the top of each session in the log records what was in
effect.

## What the log is for

The `initialize` line records what storm passed — for Brood War 1.16.1,
`programid` `'SEXP'` and `versionid` 211 (`docs/snp-abi.md` §7) — and which
program loaded the module. Each `advertising` line records
`StartAdvertisingGame`'s arguments, including the eighth, whose purpose
`storm_spi.h` still marks `UNVERIFIED`, and the extra bytes. `LockGameList asks for…` records the category filter the join screen
requests, `game list now holds…` what the module handed storm, and the
`destroy` line the traffic and how often the list was polled.

Every line carries the id of the process that wrote it, in brackets after the
time: two copies of the game run from one folder share this module and so this
log. Retail 1.16.1 will not start a second copy of itself, so two copies on one
machine means a build without that check. The
signalling server's own log names games by the ids it gives them, and players
by the first eight digits of theirs, which the module's `online as` and
`joined game` lines also show.

## Testing without the game

`tests/snp-sim` loads two copies of this module into one process and plays
storm against them: advertise, list, `GetGameInfo`, `Send`, `Receive`, `Free`,
`Destroy`, unload, reload. `ctest` runs it.
