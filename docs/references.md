# References

None of this is vendored, and none of it belongs inside this tree: clone what
you need somewhere else. The paths below are inside each repository. This
repository is MIT and stays that way; the sources below have their own
provenance and licensing, some of it unclear. Read them, cite them in comments,
do not copy code out of them.

## Primary

### Leaked Hellfire Storm source
`https://github.com/pvpgn/diablo-hellfire` — `src/Storm/`

Blizzard's own Storm source. The highest-authority material available, and the
only source for **how storm behaves** rather than what it expects.

Files that matter:

| Path | What it gives you |
|---|---|
| `H/STORM.H` | `SNETCAPS`, `SNETADDR`, `SNETSPI`, the game/device list nodes, the descriptor structs, error codes |
| `SOURCE/SNET.CPP` | Storm's side: enumeration, the caps archive check, binding, and every call into the module |
| `SOURCE/STANDARD/` | Blizzard's IPX, serial, modem and null providers — `STANDARD.CPP` is the canonical `SnpQuery`/`SnpBind` |
| `SOURCE/BATTLE/` | The Battle.net provider, including `SPI.CPP` |
| `SOURCE/IPXTEST/` | A standalone provider outside the main module — the closest thing to a worked example |
| `DOCS/*.RTF` | Blizzard's own SNet API documentation |
| `SOURCE/*/CAPS.DAT`, `CAPS.MPQ` | The capability archives appended to each module |

Caveat: this is a mid-1996-to-Hellfire snapshot. Struct sizes here are sometimes
*smaller* than the shipped Diablo 1.09 and StarCraft versions — several
descriptor structs grew fields later. Cross-check sizes against devilution.

### BWAPI `SNP_DirectIP`
`https://github.com/bwapi/bwapi` — `bwapi/SNP_DirectIP/`, `bwapi/Storm/storm.h`

The only complete public `.snp` for StarCraft 1.16.1: two providers (UDP
"Direct IP" and shared-memory "Local PC") in one module, shipped and widely
used. Authoritative for the StarCraft-era table and for the build recipe —
`SNP_DirectIP.vcxproj` contains the `copy /B dll + caps.mpq` step, and
`SNP/caps.mpq` is the archive itself.

Its `Storm/storm.h` is reverse-engineered, so its field names and comments are
guesses where Blizzard's source has real names. Where they conflict, Blizzard
wins; where the header conflicts with DirectIP's working code, the code wins.

### devilution
`https://github.com/diasurgical/devilution` — `structs.h`, `Source/nthread.cpp`

Diablo 1.09 decompilation built from Blizzard's own debug symbols, so the names
are real. `structs.h` carries the later, larger descriptor structs;
`nthread.cpp` shows how the engine consumes the caps fields.

### DevilutionX
`https://github.com/diasurgical/devilutionX` — `Source/dvlnet/`, `Source/storm/`

Not a reference for the SNP interface — it has no SNP loader — but a **target**
in its own right. See `devilutionx-integration.md`. Its `_SNETCAPS` in
`Source/storm/storm_net.hpp` carries useful annotations on what the caps fields
actually do.

### Skywing
`www.valhallalegends.com` and `www.blizzhackers.cc`, via `web.archive.org`

Long-form writing on Storm internals and the Battle.net protocol from someone
who worked at that level. Not fetchable by script — search the Wayback Machine
by hand. Context and rationale rather than field offsets.

## Secondary

- **ShieldBattery** `https://github.com/ShieldBattery/ShieldBattery` —
  `game/src/snp.rs`. A working SNP shim, but for **StarCraft: Remastered**,
  whose table is a different generation: different slot order, a four-argument
  `initialize` with no event handle, and calls with no 1.16.1 counterpart.
  Do not port offsets from it. Useful as a second opinion on caps values.
- **BNETDocs** `https://bnetdocs.org/document/34` — the Storm UDP protocol that
  rides *on top of* whatever the SNP provides. Prose, no code.
