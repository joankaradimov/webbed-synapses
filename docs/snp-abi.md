# The Storm SNP interface

Everything a `.snp` module must get right, with the source for each claim.

**Confidence markers used throughout:**

- **[S]** read from Blizzard's own Storm source (Hellfire leak) — highest.
- **[W]** read from working third-party code that ships and runs.
- **[?]** inferred, or claimed by a secondary source. Verify before relying on it.

Paths like `STORM.H:1472` refer to the Hellfire Storm tree; see
[`references.md`](references.md) for how to fetch it.

---

## 1. Module surface

A `.snp` is a 32-bit Windows DLL exporting two functions **by name**. **[S]**
`SNET.CPP:544, 680` resolves both with `GetProcAddress`; ordinals are never used,
so the `EXPORTS` block needs no ordinal numbers. Blizzard's own modules
(`STANDARD.DEF`, `BATTLE.DEF`) export exactly:

```
EXPORTS
SnpBind
SnpQuery
```

```c
// STORM.H:1506
typedef BOOL (APIENTRY *SNETSPIBIND )(DWORD index, SNETSPIPTR *spi);
typedef BOOL (APIENTRY *SNETSPIQUERY)(DWORD index, DWORD *id,
                                      LPCSTR *description, LPCSTR *requirements,
                                      SNETCAPSPTR *caps);
```

`index` is a **0-based provider index within this module**, not an id. One DLL
may expose several providers; storm walks `index` upward and stops at the first
`FALSE`. **[S]** `SNET.CPP:551-608`. Blizzard's `STANDARD.snp` returns IPX,
serial, null and modem at indices 0–3.

`id` is the FOURCC storm later passes to `SNetInitializeProvider` and matches
against when binding. Known values: `'IPXN'`, `'MODM'`, `'BNET'`/`'TENB'`,
`'DRIP'` and `'SMEM'` (BWAPI) **[W]**, `'SBAT'` (ShieldBattery, SC:R) **[W]**.

All four output pointers must stay valid until the module is unloaded — storm
copies the caps but keeps the strings. **[S]** `SNET.CPP:598-601`.

## 2. Enumeration, binding, and the caps archive

This is the part that is invisible from the outside, and it is where new
modules fail silently.

**[S]** `SNET.CPP:496-620` — `SpiFindAllProviders`:

1. Take the directory of the module that owns storm and glob `*.snp`.
2. For each match, **open the file as an MPQ archive** and read `caps.dat`
   from it (`SpiLoadCapsSignature`). If the file is not a valid archive, or
   the archive has no signature or one that fails to verify, **the provider
   is skipped entirely** — no error, no menu entry.
3. `LoadLibrary`, `GetProcAddress("SnpQuery")`, then loop `index` from 0.
4. Reject the provider unless `caps->size >= sizeof(SNETCAPS)`.
5. Filter on `SNET_CAPS_DEBUGONLY` / `SNET_CAPS_RETAILONLY` against the build.
6. In signed mode, **use the caps from `caps.dat`, not the ones `SnpQuery`
   returned** — the source comment says the returned ones are "too easy to hack".
7. Subtract `sizeof(HEADER) + sizeof(DWORD)` = **16 bytes** from
   `maxmessagesize` for storm's own header. `SNET.CPP:605`.
8. `FreeLibrary` — the module is loaded twice in its life, once to enumerate
   and once to bind. Do no expensive or stateful work in `DllMain`.

On selection, **[S]** `SNET.CPP:672-712`:

- `LoadLibrary` again, `GetProcAddress("SnpBind")`, call it with the stored index.
- Reject if `spi->size < MINSPISIZE`, where `MINSPISIZE = 20 * sizeof(DWORD)`
  = **80** — the size field plus 19 function pointers. `SNET.CPP:35`.
- `CopyMemory(dst, spi, min(spi->size, sizeof(SNETSPI)))` into a zeroed struct.
  **A module may report a smaller table than storm knows about; storm zero-fills
  the rest.** This is the compatibility mechanism.
- Call `Initialize` immediately.

> **How to build the file.** BWAPI's post-build step is literally
> `copy /Y /B module.dll + caps.mpq module.snp` **[W]**
> (`SNP_DirectIP.vcxproj:93`). `cmake/SnpModule.cmake` reproduces it.

**The archive must start on a 512-byte boundary.** **[S]** `SFILE.CPP:2088-2105`
— `SFileOpenArchive` reads a header at offset 0, and while the signature does
not match it advances the offset by 512 and reads again, until the read comes up
short. That stepping is the whole reason appending an archive to a DLL works,
and it is also a constraint: a module whose length is not a multiple of 512
hides its archive in the gap. A PE with the default `FileAlignment` is already
aligned, so this normally costs nothing; `cmake/SnpPackage.cmake` pads when it
does not.

### Enumerated is not the same as listed

**[S]** `SNET.CPP:2853-2861`. Everything above gets a provider into storm's
internal list. Getting it into the menu is a second gate, applied when the
list box is filled:

```c
if (curr->id && SpiMeetsMinimumCaps(&curr->caps, uiparams->mincaps))
    SendDlgItemMessageA(window, IDC_PROVIDERLIST, LB_ADDSTRING, 0, curr->desc);
```

and `SpiMeetsMinimumCaps` (`SNET.CPP:765-775`) demands, against the caps the
*game* passed to `SNetSelectProvider`:

```c
(mincaps->flags & curr->flags) == mincaps->flags
mincaps->maxmessagesize <= curr->maxmessagesize
mincaps->maxqueuesize   <= curr->maxqueuesize
mincaps->maxplayers     <= curr->maxplayers
mincaps->bytessec       <= curr->bytessec
!mincaps->latencyms || mincaps->latencyms >= curr->latencyms
```

So every field but latency wants to be generous, every declared flag has to be
a superset of what the game asks for, and **a provider that describes itself
modestly is dropped with no error anywhere** — indistinguishable, from the
outside, from an archive storm refused to open. Blizzard's own null provider
declares `maxplayers` 1 and an empty description; it was never meant to be
seen.

What 1.16.1 passes as `mincaps` is not known exactly, but Blizzard's own
modules bound it. Read from the `caps.dat` inside the `battle.snp` and
`standard.snp` of a 1.16.1 install (both files dated 2009-01-10):

| id | description | flags | maxmessagesize | maxplayers | bytessec | latencyms |
|---|---|---|---|---|---|---|
| `BNET` | Battle.net | **0** | 512 | 256 | **1500** | **500** |
| `IPXN` | Local Area Network (IPX) | 3 | 504 | 256 | 100000 | 50 |
| `UDPN` | Local Area Network (UDP) | 3 | 504 | 256 | 100000 | 50 |
| `SCBL` | Direct Cable Connection | 0 | 512 | 4 | 1500 | 500 |
| `MODM` | Modem | 0 | 180 | 2 | 1000 | 250 |
| `'    '` | *(empty)* | 0 | 512 | 1 | 3145728 | 0 |

(`maxqueuesize` is 16 throughout.) Battle.net is always in the menu, so
1.16.1's `mincaps` has **`flags` 0**, `maxmessagesize` ≤ 512, `bytessec` ≤ 1500,
and `latencyms` either 0 or ≥ 500. If Modem is listed too, the bounds tighten to
`maxplayers` ≤ 2, `maxmessagesize` ≤ 180 and `bytessec` ≤ 1000. Whether
`maxplayers` 1 is enough is still **[?]** — the empty-named entry is
Blizzard's own null provider, and whether 1.16.1 lists it as a blank row would
answer it.

BWAPI DirectIP's numbers pass, observed with our own module carrying them
(2026-09-16): flags 3, `maxmessagesize` 500, `maxqueuesize` 16, `maxplayers`
256, `bytessec` 1000, `latencyms` 50, 8 turns/sec, 2 in transit.

`tools/load-snp` walks a built module through the enumeration sequence and
flags the caps that commonly fail this test; it cannot know `mincaps`, so it
cannot prove the entry appears.

### What is actually in `caps.dat`

**[S]** `SNET.CPP:408-431` (`SpiExtractCaps`), confirmed byte for byte against
Blizzard's own `SOURCE/STANDARD/CAPS.DAT`. The file is a **stream of
provider records**, one per index `SnpQuery` answers, concatenated with no
outer header:

| Field | Size | Notes |
|---|---|---|
| `bytes` | DWORD | length of the rest of *this* record; storm rejects the file if it does not match |
| `id` | DWORD | the provider FOURCC, e.g. `'IPXN'` |
| `desc` | NUL-terminated string | what the network-selection menu shows |
| `req` | NUL-terminated string | the "requires..." line |
| `caps` | `caps.size` bytes | a `SNETCAPS`, §3 |

so `bytes == 4 + strlen(desc)+1 + strlen(req)+1 + caps.size`. Blizzard's
`STANDARD` archive opens with `7f 00 00 00`, `'NXPI'`, `"Local Area Network
(IPX)"`, `"All computers must be connected to an IPX-compatible network."`,
then a 36-byte `SNETCAPS` — 127 bytes, exactly as the rule requires.

**The format is the same in the StarCraft era.** **[W]** BWAPI's shipped
`caps.mpq` holds a 312-byte `caps.dat` that parses cleanly as two records with
both lengths checking out: `'DRIP'` / `"Direct IP"` and `'SMEM'` /
`"Local PC"` — the two entries StarCraft 1.16.1 actually shows in its network
menu, which is also the evidence that the menu strings come from here rather
than from `SnpQuery`. Their capabilities are worth copying: `flags` 3,
`maxmessagesize` 500, `maxqueuesize` 16, `maxplayers` 256, `bytessec` 1000,
`latencyms` 50, 8 turns/sec, 2 in transit. `tools/inspect-snp.mjs` prints all
of this from any `.snp` or archive.

`SpiExtractCaps` never learns the buffer length: it walks record to record and
the loop is bounded by `SnpQuery` returning `FALSE` (`SNET.CPP:553-590`). **The
record count must therefore match the provider count**, and the two live in
different files, so a build should generate them from one source.

In signed mode the `id`, `desc` and `req` storm uses come from **`caps.dat`,
not from `SnpQuery`** — the source says the returned ones are "too easy to
hack", and `SnpQuery`'s answer is used only as a yes/no liveness check plus the
`SNET_CAPS_DEBUGONLY` / `RETAILONLY` filter.

**Observed in StarCraft 1.16.1** (2026-09-16). The null provider's DLL — whose
`SnpQuery` answers "Null (no network)" — was packaged with BWAPI's archive and
installed beside BWAPI's real DirectIP (`SNP.snp`). The menu showed **"Direct
IP" twice**: once for BWAPI's module and once for ours. 1.16.1 is a
signed-mode build and takes the menu strings from `caps.dat`. The build
generates both from one declaration, so they never disagree in practice.

The archives themselves are small: Blizzard's `STANDARD/CAPS.MPQ` is **490
bytes** and `BATTLE/CAPS.MPQ` is **316**, both MPQ v1, 4096-byte sectors.
BWAPI's is 17 KB only because its hash table has 1024 entries; it holds three
files — `caps.dat`, `(listfile)` and a 72-byte weak `(signature)`. Both
Blizzard's and BWAPI's store `caps.dat` compressed *and* encrypted (PKWARE
implode in Blizzard's case). Ours stores it plainly, which is the simplest
thing storm's reader handles and removes a compatibility question we would
otherwise have to answer by experiment.

`cmake/SnpModule.cmake` generates the archive: `tools/caps-gen` writes
`caps.dat` from the module's own `snp_caps_records[]`, and `tools/mpq-builder`
packs the staging directory with StormLib. The declaration is shared with
`SnpQuery`, so the record count and the provider count cannot drift apart.

**On signing, what is now known.** **[W]** The weak signature covers only the
archive region — StormLib computes it from `ha->MpqPos` to
`MpqPos + ArchiveSize` (`SFileVerify.cpp:163-177`) — so an archive signed
*before* being appended to a module still verifies inside the finished `.snp`.
Measured, not deduced: a signed 312-byte archive appended to a 515 KB module
verifies, and so does BWAPI's when appended to a different module than the one
it shipped with. StormLib's weak key also verifies Blizzard's own
`STANDARD/CAPS.MPQ`, which means a signature we generate is not distinguishable
from theirs. `snp-mpq --sign` produces one, `SNP_CAPS_SIGN=ON` turns it on in
the build, and `snp-mpq --verify` reports what an archive or a finished `.snp`
carries.

> **Signing, in the game.** Storm skips a module whose archive has **no**
> signature, as well as one whose signature fails to verify **[S]**
> `SNET.CPP:731-737`. `SFileAuthenticateArchive` reports a missing
> `(signature)` file as `SFILE_AUTH_NOSIGNATURE` (1) (`SFILE.CPP:1167-1171`),
> and storm refuses every result from 1 to 4. This block used to say that
> Hellfire's storm accepts an absent signature; the source says otherwise.
>
> **1.16.1 does the same**, read from its `storm.dll` by hand on 2026-09-25.
> The provider scan authenticates each archive (the call at `0x1503DDDD`),
> accepts a result of 0 or at least 5, and skips the module otherwise
> (`0x1503DDE2`–`0x1503DDEC`). The authentication itself returns 1 when the
> hash table holds no `(signature)` (`0x15014B26`–`0x15014B3B`, the name at
> `0x15052C80`). Only then does the scan open `caps.dat`.
>
> Observed 2026-09-16: **a weak-signed archive is accepted.** The breadcrumb
> log shows storm loading three copies of our DLL, each carrying a
> weak-signed archive — our own twice, BWAPI's once — and storm reaches
> `LoadLibrary` only after the archive check passes. Blizzard's own
> `battle.snp` and `standard.snp` in the same install are weak-signed too, as
> is BWAPI's `SNP.snp`. That run's unsigned copies must have been skipped; its
> one "Null (no network)" entry came from a signed one.
>
> So the signature is required, not a precaution. The build signs by default
> (`SNP_CAPS_SIGN`), and a module built without it will not be listed.

## 3. `SNETCAPS`

**[S]** `STORM.H:1217`. Nine DWORDs, 36 bytes. Identical layout in both eras.

| # | Field | Notes |
|---|---|---|
| 0 | `size` | must be `>= sizeof(SNETCAPS)` or the provider is dropped |
| 1 | `flags` | see below |
| 2 | `maxmessagesize` | storm subtracts 16 from this |
| 3 | `maxqueuesize` | 16 in every Blizzard provider |
| 4 | `maxplayers` | clamped against `programdata->maxplayers` |
| 5 | `bytessec` | drives storm's delta/message sizing |
| 6 | `latencyms` | **drives every storm timeout** — see below |
| 7 | `defaultturnssec` | |
| 8 | `defaultturnsintransit` | |

Flags **[S]** `STORM.H:1102`:

```c
#define SNET_CAPS_PAGELOCKEDBUFFERS  0x00000001  // storm VirtualLocks an 8 KB send buffer
#define SNET_CAPS_BASICINTERFACE     0x00000002
#define SNET_CAPS_DEBUGONLY          0x10000000
#define SNET_CAPS_RETAILONLY         0x20000000
```

BWAPI's `0x20000003` is exactly `PAGELOCKEDBUFFERS | BASICINTERFACE |
RETAILONLY` — the same value Blizzard's IPX provider ships in a release build
**[S]** `IPX.CPP:1340`. The field is not mysterious.

`latencyms` is load-bearing **[S]** `SNET.CPP:645-650`:

```c
timetoackturn = max( 250,  5 * latencyms);
timetoblock   = max(5000, 12 * latencyms);
timetogiveup  = max(1000,  4 * latencyms);
timetorequest = max(  25,      latencyms / 2);
timetoresend  = max(  50,      latencyms);
```

Reference values actually shipped:

| | IPX **[S]** | BWAPI DirectIP **[W]** |
|---|---|---|
| `maxmessagesize` | 504 | 500 |
| `maxqueuesize` | 16 | 16 |
| `maxplayers` | 256 | 256 |
| `bytessec` | 100000 | 1000 |
| `latencyms` | 50 | 50 |
| `defaultturnssec` | 8 | 8 |
| `defaultturnsintransit` | 2 | 2 |

## 4. The function table

**[S]** `STORM.H:1472`. Slots 1–19 are alphabetical — a Blizzard header
artifact, and the reason both eras agree on them.

| # | Off | Name | Signature (`CALLBACK`, returns `BOOL`) |
|---|---|---|---|
| — | 0x00 | `size` | `DWORD` |
| 1 | 0x04 | `CompareNetAddresses` | `(SNETADDRPTR, SNETADDRPTR, DWORD *result)` |
| 2 | 0x08 | `Destroy` | `()` |
| 3 | 0x0C | `Free` | `(SNETADDRPTR, LPVOID, DWORD)` |
| 4 | 0x10 | `FreeExternalMessage` | `(LPCSTR, LPCSTR, LPCSTR)` |
| 5 | 0x14 | `GetGameInfo` | `(DWORD, LPCSTR, LPCSTR, SNETSPI_GAMELIST *)` |
| 6 | 0x18 | `GetPerformanceData` | `(DWORD, DWORD *, LARGE_INTEGER *, LARGE_INTEGER *)` |
| 7 | 0x1C | `Initialize` | `(PROGRAMDATA, PLAYERDATA, UIDATA, VERSIONDATA, HANDLE)` |
| 8 | 0x20 | `InitializeDevice` | `(DWORD, PROGRAMDATA, PLAYERDATA, UIDATA, VERSIONDATA)` |
| 9 | 0x24 | `LockDeviceList` | `(SNETSPI_DEVICELISTPTR *)` |
| 10 | 0x28 | `LockGameList` | `(DWORD, DWORD, SNETSPI_GAMELISTPTR *)` |
| 11 | 0x2C | `Receive` | **era-dependent, see below** |
| 12 | 0x30 | `ReceiveExternalMessage` | `(LPCSTR *, LPCSTR *, LPCSTR *)` |
| 13 | 0x34 | `SelectGame` | `(DWORD, PROGRAMDATA, PLAYERDATA, UIDATA, VERSIONDATA, DWORD *)` |
| 14 | 0x38 | `Send` | `(DWORD count, SNETADDRPTR *list, LPVOID, DWORD)` |
| 15 | 0x3C | `SendExternalMessage` | `(LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR)` |
| 16 | 0x40 | `StartAdvertisingGame` | **era-dependent arity, see below** |
| 17 | 0x44 | `StopAdvertisingGame` | `()` |
| 18 | 0x48 | `UnlockDeviceList` | `(SNETSPI_DEVICELISTPTR)` |
| 19 | 0x4C | `UnlockGameList` | `(SNETSPI_GAMELISTPTR, DWORD *)` |

Slot 19 is the last one storm requires (`MINSPISIZE` = 0x50). Beyond it the
eras diverge:

- **Diablo 1.05** adds slot 20 `GetLocalPlayerName(LPSTR, DWORD, LPSTR, DWORD)`
  **[S]**.
- **StarCraft** keeps slot 20 and adds slots 21–26 (report result, check data
  file, league commands) and slot 27 `LeagueGetName(char *, DWORD)` **[W]**.
  BWAPI's header calls slot 20 `StartAdvertisingGame(name, namelen, pw,
  pwlen)`. DirectIP leaves 20–26 `NULL` and implements only 27.

**Slot 20 is the same function in both eras.** 1.16.1's `storm.dll` calls it
exactly as Diablo's `GetLocalPlayerName`. When `SNetCreateLadderGame` is given
an empty player name, storm takes slot 20 from the bound table (the global at
`0x1505E620`, offset `0x50`) and, if it is not `NULL`, calls it as
`(name, 128, desc, 128)` into two buffers of its own. It then uses what comes
back as the player's name and description (`0x1503AF0E`–`0x1503AF45`, read by
hand on 2026-09-25). BWAPI's name for the slot is wrong. `storm_spi.h` keeps
BWAPI's field name, `StartAdvertisingGameShort`, only because
`tests/abi_layout.cpp` asserts on it. Only slots 21–27 are StarCraft's own.

### Slot 11, `Receive` — the era trap

```c
// STORM.H:1484 — Blizzard's comment, verbatim:
//   note: this is the way that the Receive call should look...
//     BOOL (CALLBACK *Receive)(SNETADDRPTR *,LPVOID *,DWORD *);
//   below is the receive call with two parameters switched around
//   to make it incompatible with older .snp's during the beta...
BOOL (CALLBACK *Receive)(LPVOID *data, DWORD *databytes, SNETADDRPTR *addr);
```

The swapped form is the one Hellfire's storm actually calls **[S]**
(`SNET.CPP:2276`; the "restore this" version is commented out beside it).
StarCraft-era storm uses the *unswapped* order **[W]**. Same slot, incompatible
contract — this cannot be papered over, only branched on.

Whether retail Diablo 1.09 kept the swap is **[?]**. Test before assuming.

### Slot 16, `StartAdvertisingGame` — arity

Diablo, 9 args **[S]**: `(name, password, description, gamemode, age,
categorybits, optcategorybits, clientdata, clientdatabytes)` — storm's own call
passes `age` as 0, and builds the client data as its 12-byte
`CLIENTDATA {bytes, numplayers, maxplayers}` (`SNET.CPP:67, 1456, 3067-3082`).
StarCraft, 10 args **[W]**: an extra parameter, and the reference implementation
names it `StartAdvertisingLadderGame` despite it being the ordinary
advertising path.

## 5. Data structures

```c
// STORM.H:1443
typedef struct _SNETADDR { BYTE address[16]; } SNETADDR;
```

Sixteen opaque bytes. Storm never interprets them; a `SOCKADDR` fits exactly,
which is why UDP providers use one directly. Anything that fits in 16 bytes and
compares bytewise is a legal address.

```c
// STORM.H:1456 — 308 bytes
typedef struct _SNETSPI_GAMELIST {
    DWORD    gameid;            // 0x000
    DWORD    gamemode;          // 0x004
    DWORD    creationtime;      // 0x008
    SNETADDR owner;             // 0x00C
    DWORD    ownerlatency;      // 0x01C
    DWORD    ownerlasttime;     // 0x020
    DWORD    gamecategorybits;  // 0x024
    char     gamename[128];     // 0x028
    char     gamedescription[128]; // 0x0A8
    struct _SNETSPI_GAMELIST *next; // 0x128
    LPVOID   clientdata;        // 0x12C  (new in 1.05)
    DWORD    clientdatabytes;   // 0x130  (new in 1.05)
} SNETSPI_GAMELIST;
```

StarCraft appends `product` and `version` DWORDs, making it 316 bytes **[W]**.
The reverse-engineered SC header calls several of these fields "unknown"; they
are the ones named above, in the same order.

`SNETSPI_MAXCLIENTDATA` is 256 **[S]** — the ceiling on the per-ad blob, though
DirectIP only reserves 32 bytes for it **[W]**.

`Initialize` receives four descriptor structs — `SNETPROGRAMDATA`,
`SNETPLAYERDATA`, `SNETUIDATA`, `SNETVERSIONDATA` **[S]** `STORM.H:1150+`.
Note that these *grew over time*: Hellfire's `SNETPROGRAMDATA` has 11 fields;
the Diablo 1.09 and StarCraft versions have 15 (CD key, owner, shareware flag,
LCID appended). Read `size` — do not assume.

## 6. Contracts

**Call order** **[W]** (DirectIP's own notes, consistent with `SNET.CPP`):

```
Initialize                       // storm hands over the receive event
LockDeviceList / UnlockDeviceList// once; return an empty list and storm stops asking
LockGameList / UnlockGameList    // polled roughly once a second
StartAdvertisingGame             // when hosting starts
StopAdvertisingGame              // when hosting stops
GetGameInfo                      // for the game about to be joined
Send / Receive / Free            // in-game traffic
Destroy                          // on return to the provider menu
```

**Receive ownership.** The module allocates; the module frees. `Receive` returns
two interior pointers into one block, and `Free(addr, data, databytes)` must
recover that block — in practice from the `addr` pointer, so the address field
goes first in the packet struct. **[W]**

"Nothing waiting" is not a bare `FALSE`: set
`SNET_ERROR_NO_MESSAGES_WAITING = 0x8510006B` **[S]**
(`STORMERROR(107)`, `STORM.H:32,41`) and return `FALSE`.

**`CompareNetAddresses`.** The out parameter is named `diffmagnitude`, but
Blizzard's own null provider writes `!memcmp(...)` — **1 when equal** **[S]**
(`NULL.CPP:31`). BWAPI does the same **[W]**. Ignore the parameter name.

**`Send` fan-out.** Storm caps it at 16 addresses and 8192 bytes
**[S]** `SNET.CPP:856`, and does use more than one. Handle the whole list.

**Storm gossips peer addresses, and will ask you to send to a stranger.** This
is the single most consequential contract in the whole interface for anything
that is not a LAN broadcast transport, and it is easy to miss.

`SysBuildPlayerInfo` (`SNET.CPP:1674`) copies each connection's `SNETADDR` into
a PLAYERINFO system message that goes out over the wire **[S]**:

```c
data->playerid  = conn->playerid;
CopyMemory(&data->addr, &conn->addr, sizeof(SNETADDR));
```

`SysOnPlayerInfo` (`SNET.CPP:1786`) receives it and, for a **third party**,
builds a connection record from the address carried in the message body — then
storm sends to it. So addresses propagate player-to-player as opaque 16-byte
blobs, and **the module is routinely asked to `Send` to a peer it has never
seen and never negotiated anything with**.

The same function carries Blizzard's own note on the other half of it:

> IF THIS IS THE PLAYER INFO FOR THE SYSTEM SENDING THE PACKET, USE THE PACKET'S
> ORIGIN ADDRESS RATHER THAN THE ONE IN THE PLAYER INFORMATION, BECAUSE THE
> SENDER PRESUMABLY DOESN'T KNOW HIS OWN ADDRESS.

A peer does not know its own address; storm substitutes the source address the
receiver observed. Between the two, every participant must end up agreeing on
the same sixteen bytes for a given player.

Three consequences for any transport that is not "IP address on a LAN":

1. **`SNETADDR` has to be a globally meaningful, stable peer identity**, minted
   once and identical for everyone. An IP and port cannot do this job off a LAN,
   which is exactly why DirectIP is direct-only.
2. **Connection establishment must be lazy and asynchronous.** `Send` is
   synchronous and must not block; a send to an unknown peer starts whatever
   negotiation is needed and returns. Storm retransmits, so losing the first
   packets is survivable.
3. **The mesh forms itself.** No orchestration is needed: a new player is
   announced by gossip, every existing peer's storm then sends to them, and each
   module connects on demand.

**Threading.** Blizzard's providers guard their state; DirectIP wraps every
entry point in a critical section, which is direct evidence storm calls in on
more than one thread **[W]**. Exactly which threads is **[?]**. Assume
concurrent entry and lock accordingly. DirectIP creates no I/O thread at all —
it polls the socket from inside `Receive` and refreshes ads from inside
`UnlockGameList`.

**The receive event.** `Initialize`'s last parameter is a `HANDLE` storm waits
on. `SetEvent` it whenever a packet is queued.

### What storm needs from a game

Observed on 2026-09-24, driving the real 1.16.1 `storm.dll` from a console
client the way a game does: two to four processes creating, listing, joining
and playing lockstep games over `webbed.snp`. None of it concerns the module,
but all of it bites anything that drives storm without the game.

- **Nothing is found until `SFileDestroy()` has been called.** Storm reads each
  `.snp` through its own file layer, and without that call its provider scan
  comes back empty and every `SNetInitializeProvider` fails with
  `ERROR_BAD_PROVIDER` (see also
  [bwapi#375](https://github.com/bwapi/bwapi/issues/375)).
- **Storm looks for `*.snp` only in its own directory.**
- **A player name may never be empty.** `SNetCreateGame` and `SNetJoinGame`
  return without releasing storm's lock when it is, and storm stops.
- **A join is turned away across program id or version.**
- **`SNetSetGameMode` takes two arguments**, not the one `STORM.H` gives it.
- **Storm never drops a player who has vanished.** The game has to notice —
  `SNetReceiveTurns` fills in each player's status even when it returns
  nothing — and call `SNetDropPlayer`.
- **Leaving tears down at once**, turns still owed to other players included.
- **Blizzard's `standard.snp` runs one copy of its UDP provider per machine.**
  The second fails `SNetInitializeProvider` with `0x4DC`, most likely on its
  fixed port.

## 7. Version signals

> **Mostly moot.** There is one SNP target — StarCraft 1.16.1 — because the
> Diablo-side target is DevilutionX, which loads no `.snp` at all (AGENT.md §4).
> Nothing here needs to be built. It is kept because `programid` is still worth
> reading at `Initialize`, for the reason in the last paragraph.

What the host tells the module about itself, and when.

**Before `SnpBind` — nothing in-band.** `SnpQuery` and `SnpBind` receive only a
provider index. Anything learned this early is a sniff of the process: the
`VERSIONINFO` resource on the loaded `storm.dll` (`GetModuleHandle("storm.dll")`),
or the host executable's name and version. Usable as a hint, unreliable under
launchers and repacks, and **[?]** — nobody has confirmed which Blizzard builds
carry a usable version resource.

**At `Initialize` — decisive, but too late to choose a function table.** Storm
has already copied the table by then (`SNET.CPP:696`), so this can validate an
era, never select one.

| Signal | Discriminates | Confidence |
|---|---|---|
| `programdata->programid` | the game itself, exactly | **[S]** field exists; `SEXP` observed, other codes **[?]** |
| `programdata->versionid` | patch level within a game | **[S]** |
| `programdata->size` | 44 on the Hellfire snapshot, 60 on Diablo 1.09 **and** StarCraft | **[S]**/**[W]** |
| `playerdata->size` | 16 throughout | **[S]** |
| `interfacedata->size` | 64 on the Hellfire snapshot, 80 on 1.09 | **[S]**/**[W]** |

`programdata->size == 60` is stated outright in the reverse-engineered StarCraft
header **[W]**, and the struct there is field-for-field the 15-field
`SNETPROGRAMDATA` **[W]**. So **size alone does not separate Diablo 1.09 from
StarCraft** — only `programid` does.

Blizzard's own providers cache exactly these three at `Initialize` and use
`programid` to filter foreign advertisements off the wire **[S]**
(`IPX.CPP:936-938, 265`). Do the same: a Diablo ad and a StarCraft ad on the
same LAN must not appear in each other's game lists.

Product codes are the well-known Battle.net FOURCCs — `DRTL`/`DSHR` Diablo,
`HRTL`/`HSHR` Hellfire, `STAR`/`SSHR` StarCraft, `SEXP` Brood War, `W2BN`
Warcraft II BNE, `D2DV`/`D2XP` Diablo II. They appear in none of the sources
above, so treat the list as **[?]** and confirm each against the game you
actually target.

**Observed, StarCraft: Brood War 1.16.1** (2026-09-17, the WebRTC module's log):
`programid` **0x53455850** (`'SEXP'`), `versionid` **211**, `maxplayers` 8,
`programdata->size` 60. `playerdata->playername` is **empty** at `Initialize`
— the player picks a name later, so a module cannot learn it this early.

**What StarCraft advertises** (same run, creating a game): `StartAdvertisingGame`
received name `"Chaos"`, no password, state 0, elapsed 0, **category bits**
`0x4000F`, **optional category bits** `0xFF` — the `optcategorybits` StarCraft
puts in its `SNETPROGRAMDATA` — parameter 8 `0`, 12 extra bytes (storm's
`CLIENTDATA`), and the stat string
`,44,,6,,f,,4,5ab5438b,10,,Chaos\rUntitled Scenario\r` — comma-separated fields
then the host's name and the map's name, each **terminated by CR**. Those CRs
are structure: a listing whose stat string has lost them is dropped by the
joiner's game without a word. DirectIP hard-codes `0xA7` as the listing's
category bits instead of passing the host's through **[W]**, and its games are
listed; this module copies it.

**Parameter 8** is the fifth argument of `SNetCreateLadderGame`, which storm
keeps and passes on. In 1.16.1's `storm.dll`, `SNetCreateLadderGame` stores its
fifth argument at `0x1505BC90` (`0x1503AFFB`–`0x1503AFFE`), and the advertising
call pushes that global as the eighth argument of slot 16 (`0x15037BC2`, the
call at `0x15037BF8`); both were read by hand on 2026-09-25.
StarCraft's multiplayer create path, the function at `0x4D3910` in 1.16.1's
`StarCraft.exe`, fills it from the game's tournament mode: a jump table at
`0x4D3B40` turns 0, 1, 2, 3 into 0, 1, 3, 5, read by hand on 2026-10-03. For
some game types it comes from the game's template instead **[?]**.
An ordinary game passes 0, as observed. What a provider should do with it is
still unknown; Battle.net's ladder is the likely consumer **[?]**.

**What the join screen accepts** (2026-09-24, read from 1.16.1's
`StarCraft.exe`). Storm itself leaves out of the list only games whose mode is
private, full or unjoinable **[S]** (`SNET_GM_UNLISTEDMASK`, `STORM.H:1169`;
`SNET.CPP:1502, 1517`). Everything else reaches the game, and the game filters
again. The join screen gets its games from `SNetEnumGamesEx(0, 0, …)` alone,
once a second. Its callback (`0x4D35E0`) runs each game's stat string through
StarCraft's parser (`0x472570`, checks at `0x472500`) and drops, without a
word, every game that fails. The parser requires:

- map width and height at most 256 each: one hex digit apiece, in units of 32
- active players at most the maximum, and the maximum at most 8
- speed at most 6, and approval below 7
- **tileset (field 10) below 8**
- a host name of 1–24 characters, then CR; a map name of 1–31, then CR; and
  nothing after that

Nothing else in the listing storm hands over is looked at. The game id, extra
bytes, category bits, latency and owner can be anything, so the category bits
above make no difference to the listing either way.

The stat string above fails this parser: its field 10 is `10`, tileset 16. That
was the join-screen failure of 2026-09-17. Retail 1.16.1 will not start twice
on one machine (`CheckForOtherInstances`), so at least one side of that
two-instance run was a modified StarCraft build without that check, and the
host was: retail advertises tilesets 0–7, and that build had extended tilesets
in progress and advertised its own index. So the values under "What StarCraft
advertises" are that build's game code over retail `storm.dll`, not retail
StarCraft's. A map with a classic tileset was listed and joined on
2026-09-24.

## 8. Open questions

1. ~~Does 1.16.1's storm *require* a signature?~~ **Answered 2026-09-25:
   yes.** An archive with no `(signature)` is skipped, in Hellfire's source
   and in 1.16.1's binary alike (§2).
2. What `mincaps` does 1.16.1 pass to `SNetSelectProvider`? It decides which
   enumerated providers reach the menu, and a module that fails it fails
   invisibly (§2).
3. What is slot 16's extra StarCraft parameter? Where it comes from is known
   now: `SNetCreateLadderGame`'s fifth argument, StarCraft's tournament mode
   (§7). What a provider should do with it is not.
4. `maxmessagesize` bounds: a reverse-engineered header claims 128–512 and a
   port claims underflow below 24. Blizzard's own providers use 180–512. **[?]**
   StarCraft itself carries on with a provider only if the size storm reports,
   16 less than the caps declare (§2), is at least 128, and it never sends a
   turn larger than 512 (`StarCraft.exe` at `0x4D3E85`–`0x4D3EBC`, read by
   hand on 2026-10-03).
