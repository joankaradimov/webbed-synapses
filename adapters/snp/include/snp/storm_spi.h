/*
 * storm_spi.h — the interface storm.dll expects from a .snp module.
 *
 * This is a transcription, not a design. Every declaration below is taken from
 * a reference source and cited; see docs/snp-abi.md for the reasoning and
 * docs/references.md for the sources. Citations of the form STORM.H:1472 refer
 * to Blizzard's own Storm source (Hellfire leak); [BWAPI] marks things known
 * only from the StarCraft 1.16.1 reference module.
 *
 * Layout here is load-bearing. Do not add, remove, or reorder fields. The
 * assertions in tests/abi_layout.cpp exist to stop you.
 */
#ifndef SNP_STORM_SPI_H
#define SNP_STORM_SPI_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include "snp/detail/win32_shim.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Portable compile-time assertion, usable from C and C++. */
#if defined(__cplusplus) && __cplusplus >= 201103L
#define SNP_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define SNP_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define SNP_STATIC_ASSERT(cond, msg) typedef char snp_assert_##__LINE__[(cond) ? 1 : -1]
#endif

/* ------------------------------------------------------------------ *
 *  Error codes                                    STORM.H:31-42
 * ------------------------------------------------------------------ */

#define SNP_STORMFAC              0x510
#define SNP_STORMERROR(code)      (0x80000000u | (SNP_STORMFAC << 16) | ((code) & 0xFFFFu))

#define SNP_ERROR_GAME_NOT_FOUND       SNP_STORMERROR(104)
#define SNP_ERROR_NO_MESSAGES_WAITING  SNP_STORMERROR(107)  /* 0x8510006B */
#define SNP_ERROR_NOT_IMPLEMENTED      SNP_STORMERROR(115)

/* ------------------------------------------------------------------ *
 *  Capabilities                                   STORM.H:1102, 1217
 * ------------------------------------------------------------------ */

#define SNET_CAPS_PAGELOCKEDBUFFERS  0x00000001u /* storm VirtualLocks an 8 KB send buffer */
#define SNET_CAPS_BASICINTERFACE     0x00000002u
#define SNET_CAPS_DEBUGONLY          0x10000000u
#define SNET_CAPS_RETAILONLY         0x20000000u

typedef struct _SNETCAPS {
    DWORD size;                  /* must be >= sizeof(SNETCAPS) or storm drops us */
    DWORD flags;
    DWORD maxmessagesize;        /* storm subtracts 16 from this   SNET.CPP:605  */
    DWORD maxqueuesize;
    DWORD maxplayers;
    DWORD bytessec;
    DWORD latencyms;             /* drives every storm timeout     SNET.CPP:645  */
    DWORD defaultturnssec;
    DWORD defaultturnsintransit;
} SNETCAPS, *SNETCAPSPTR;

/* ------------------------------------------------------------------ *
 *  Addressing                                     STORM.H:1440-1454
 * ------------------------------------------------------------------ */

#define SNETSPI_MAXCLIENTDATA    256
#define SNETSPI_MAXSTRINGLENGTH  128

/* Sixteen opaque bytes. Storm never interprets them; a SOCKADDR fits exactly. */
typedef struct _SNETADDR {
    BYTE address[16];
} SNETADDR, *SNETADDRPTR;

typedef struct _SNETSPI_DEVICELIST {
    DWORD    deviceid;
    SNETCAPS devicecaps;
    char     devicename[SNETSPI_MAXSTRINGLENGTH];
    char     devicedescription[SNETSPI_MAXSTRINGLENGTH];
    DWORD    reserved;
    struct _SNETSPI_DEVICELIST *next;
} SNETSPI_DEVICELIST, *SNETSPI_DEVICELISTPTR;

/*
 * Game-list node, Diablo/Hellfire era: 308 bytes.   STORM.H:1456
 * StarCraft appends two DWORDs — see SNP_GAMELIST_SC below.
 */
typedef struct _SNETSPI_GAMELIST {
    DWORD    gameid;                                /* 0x000 */
    DWORD    gamemode;                              /* 0x004 */
    DWORD    creationtime;                          /* 0x008 */
    SNETADDR owner;                                 /* 0x00C */
    DWORD    ownerlatency;                          /* 0x01C */
    DWORD    ownerlasttime;                         /* 0x020 */
    DWORD    gamecategorybits;                      /* 0x024 */
    char     gamename[SNETSPI_MAXSTRINGLENGTH];     /* 0x028 */
    char     gamedescription[SNETSPI_MAXSTRINGLENGTH]; /* 0x0A8 */
    struct _SNETSPI_GAMELIST *next;                 /* 0x128 */
    LPVOID   clientdata;                            /* 0x12C  new in 1.05 */
    DWORD    clientdatabytes;                       /* 0x130  new in 1.05 */
} SNETSPI_GAMELIST, *SNETSPI_GAMELISTPTR;

/*
 * StarCraft-era game-list node: 316 bytes. [BWAPI]
 * Identical through clientdatabytes; the reverse-engineered SC header calls
 * most of these fields "unknown" — the names above are Blizzard's.
 */
typedef struct _SNP_GAMELIST_SC {
    DWORD    gameid;
    DWORD    gamemode;
    DWORD    creationtime;
    SNETADDR owner;
    DWORD    ownerlatency;
    DWORD    ownerlasttime;                         /* 0x020  GetTickCount at refresh */
    DWORD    gamecategorybits;
    char     gamename[SNETSPI_MAXSTRINGLENGTH];
    char     gamedescription[SNETSPI_MAXSTRINGLENGTH];
    struct _SNP_GAMELIST_SC *next;
    LPVOID   clientdata;
    DWORD    clientdatabytes;
    DWORD    product;                               /* 0x134  StarCraft only */
    DWORD    version;                               /* 0x138  StarCraft only */
} SNP_GAMELIST_SC, *SNP_GAMELIST_SCPTR;

/* ------------------------------------------------------------------ *
 *  Descriptors passed to Initialize               STORM.H:1150+
 *
 *  These grew over time: Hellfire's SNETPROGRAMDATA has 11 fields, Diablo
 *  1.09 and StarCraft have 15. The layout below is the later one; the first
 *  11 fields are identical. Always read `size` rather than assuming.
 * ------------------------------------------------------------------ */

typedef struct _SNETPROGRAMDATA {
    DWORD  size;
    LPCSTR programname;
    LPCSTR programdescription;
    DWORD  programid;
    DWORD  versionid;
    DWORD  reserved1;
    DWORD  maxplayers;
    LPVOID initdata;
    DWORD  initdatabytes;
    LPVOID reserved2;
    DWORD  optcategorybits;
    /* --- added after the Hellfire snapshot --- */
    LPCSTR cdkey;
    LPCSTR registereduser;
    DWORD  spawned;
    DWORD  lcid;
} SNETPROGRAMDATA, *SNETPROGRAMDATAPTR;

typedef struct _SNETPLAYERDATA {
    DWORD  size;
    LPCSTR playername;
    LPCSTR playerdescription;
    LPCSTR displayedfields;   /* new in 1.05 */
} SNETPLAYERDATA, *SNETPLAYERDATAPTR;

/*
 * UI callbacks. Deliberately opaque past the first three fields: the callback
 * signatures differ between eras and a provider that renders no UI of its own
 * never calls them. Fill them in — with citations — if and when you need one.
 */
typedef struct _SNETUIDATA {
    DWORD  size;
    DWORD  uiflags;
    HWND   parentwindow;
    LPVOID callbacks[17];
} SNETUIDATA, *SNETUIDATAPTR;

typedef struct _SNETVERSIONDATA {
    DWORD  size;
    LPCSTR versionstring;
    LPCSTR executablefile;
    LPCSTR originalarchivefile;
    LPCSTR patcharchivefile;
} SNETVERSIONDATA, *SNETVERSIONDATAPTR;

/* ------------------------------------------------------------------ *
 *  The function table                             STORM.H:1472
 *
 *  Slots 1-19 are alphabetical (a Blizzard header artifact) and identical in
 *  both eras. Storm requires at least MINSPISIZE bytes and zero-fills the
 *  rest, so a module may report a table smaller than storm knows about.
 *  SNET.CPP:35, 690-697.
 * ------------------------------------------------------------------ */

#define SNP_MINSPISIZE  (20 * sizeof(DWORD))  /* 80: size + 19 pointers */

/*
 * Diablo / Hellfire era.
 *
 * HISTORICAL REFERENCE, NOT A BUILD TARGET. Nothing we ship binds to this
 * table: the Diablo-side target is DevilutionX, which has no SNP loader (see
 * AGENT.md section 4). It is kept because it is Blizzard's own declaration, it
 * makes the StarCraft table's provenance checkable, and the cross-table offset
 * assertions in tests/abi_layout.cpp are what prove slots 1-19 are shared.
 *
 * Note slot 11. Blizzard swapped Receive's parameters "to make it incompatible
 * with older .snp's during the beta" (STORM.H:1484) and shipped the swapped
 * form — SNET.CPP:2276 calls Receive(&data, &databytes, &addr). Whether retail
 * Diablo 1.09 kept the swap is UNVERIFIED.
 */
typedef struct _SNETSPI_DIABLO {
    DWORD size;
    BOOL (CALLBACK *CompareNetAddresses)(SNETADDRPTR, SNETADDRPTR, DWORD *);
    BOOL (CALLBACK *Destroy)(void);
    BOOL (CALLBACK *Free)(SNETADDRPTR, LPVOID, DWORD);
    BOOL (CALLBACK *FreeExternalMessage)(LPCSTR, LPCSTR, LPCSTR);
    BOOL (CALLBACK *GetGameInfo)(DWORD, LPCSTR, LPCSTR, SNETSPI_GAMELIST *);
    BOOL (CALLBACK *GetPerformanceData)(DWORD, DWORD *, LARGE_INTEGER *, LARGE_INTEGER *);
    BOOL (CALLBACK *Initialize)(SNETPROGRAMDATAPTR, SNETPLAYERDATAPTR, SNETUIDATAPTR,
                                SNETVERSIONDATAPTR, HANDLE);
    BOOL (CALLBACK *InitializeDevice)(DWORD, SNETPROGRAMDATAPTR, SNETPLAYERDATAPTR,
                                      SNETUIDATAPTR, SNETVERSIONDATAPTR);
    BOOL (CALLBACK *LockDeviceList)(SNETSPI_DEVICELISTPTR *);
    BOOL (CALLBACK *LockGameList)(DWORD, DWORD, SNETSPI_GAMELISTPTR *);
    BOOL (CALLBACK *Receive)(LPVOID *, DWORD *, SNETADDRPTR *);   /* swapped: see above */
    BOOL (CALLBACK *ReceiveExternalMessage)(LPCSTR *, LPCSTR *, LPCSTR *);
    BOOL (CALLBACK *SelectGame)(DWORD, SNETPROGRAMDATAPTR, SNETPLAYERDATAPTR,
                                SNETUIDATAPTR, SNETVERSIONDATAPTR, DWORD *);
    BOOL (CALLBACK *Send)(DWORD, SNETADDRPTR *, LPVOID, DWORD);   /* up to 16 addresses */
    BOOL (CALLBACK *SendExternalMessage)(LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR);
    BOOL (CALLBACK *StartAdvertisingGame)(LPCSTR, LPCSTR, LPCSTR, DWORD, DWORD, DWORD,
                                          DWORD, LPCVOID, DWORD);
    BOOL (CALLBACK *StopAdvertisingGame)(void);
    BOOL (CALLBACK *UnlockDeviceList)(SNETSPI_DEVICELISTPTR);
    BOOL (CALLBACK *UnlockGameList)(SNETSPI_GAMELISTPTR, DWORD *);
    /* --- new in 1.05; beyond MINSPISIZE, safe to leave NULL --- */
    BOOL (CALLBACK *GetLocalPlayerName)(LPSTR, DWORD, LPSTR, DWORD);
} SNETSPI_DIABLO, *SNETSPI_DIABLOPTR;

/*
 * StarCraft 1.16.1 era. [BWAPI]
 *
 * Slots 1-19 match SNETSPI_DIABLO exactly, with two contract differences:
 *   - Receive takes its parameters in the unswapped order.
 *   - StartAdvertisingGame takes a tenth parameter. The reference module names
 *     it StartAdvertisingLadderGame, but it is the ordinary advertising path.
 *     Parameters 6 and 7 are the game's category bits and the program's
 *     optional category bits: storm's own call passes them there
 *     (SNET.CPP:3067-3082), and Brood War 1.16.1 passed 0x4000F and 0xFF --
 *     0xFF being the optcategorybits its SNETPROGRAMDATA carries. Parameter
 *     8 is SNetCreateLadderGame's fifth argument, which 1.16.1's storm keeps
 *     and passes through (read from the binary, docs/snp-abi.md section 7);
 *     it was 0, and what a provider should do with it is UNVERIFIED.
 *     The extra bytes are storm's CLIENTDATA {bytes, numplayers, maxplayers}
 *     (SNET.CPP:67). Observed 2026-09-17, docs/snp-abi.md section 7.
 * Slot 20 is GetLocalPlayerName, as in the Diablo table: 1.16.1 calls it as
 * (name, 128, desc, 128) when a game is created with an empty player name
 * (docs/snp-abi.md section 4). The field keeps BWAPI's name, which is wrong,
 * because tests/abi_layout.cpp asserts on it. Slots 20-26 may be NULL; the
 * reference module implements only LeagueGetName.
 */
typedef struct _SNETSPI_SC {
    DWORD size;
    BOOL (CALLBACK *CompareNetAddresses)(SNETADDRPTR, SNETADDRPTR, DWORD *);
    BOOL (CALLBACK *Destroy)(void);
    BOOL (CALLBACK *Free)(SNETADDRPTR, LPVOID, DWORD);
    BOOL (CALLBACK *FreeExternalMessage)(SNETADDRPTR, LPVOID, DWORD);
    BOOL (CALLBACK *GetGameInfo)(DWORD, LPCSTR, DWORD, SNP_GAMELIST_SC *);
    BOOL (CALLBACK *GetPerformanceData)(DWORD, DWORD *, DWORD, DWORD);
    BOOL (CALLBACK *Initialize)(SNETPROGRAMDATAPTR, SNETPLAYERDATAPTR, SNETUIDATAPTR,
                                SNETVERSIONDATAPTR, HANDLE);
    BOOL (CALLBACK *InitializeDevice)(DWORD, LPVOID, LPVOID, DWORD *, LPVOID);
    BOOL (CALLBACK *LockDeviceList)(DWORD *);
    BOOL (CALLBACK *LockGameList)(DWORD, DWORD, SNP_GAMELIST_SC **);
    BOOL (CALLBACK *Receive)(SNETADDRPTR *, LPVOID *, DWORD *);   /* unswapped */
    BOOL (CALLBACK *ReceiveExternalMessage)(SNETADDRPTR *, LPVOID *, DWORD *);
    BOOL (CALLBACK *SelectGame)(DWORD, SNETPROGRAMDATAPTR, SNETPLAYERDATAPTR,
                                SNETUIDATAPTR, SNETVERSIONDATAPTR, DWORD *);
    /* An array of `count` address pointers: SNETADDR **, as in the Diablo
     * table above [S] and as DirectIP dereferences it, *(addrList[0]) [W].
     * This line said SNETADDRPTR ** -- one indirection too many -- until the
     * first provider that actually read the list failed to compile against
     * it. Every argument is pointer-sized either way, so the stack layout was
     * never wrong; only the type. */
    BOOL (CALLBACK *Send)(DWORD, SNETADDRPTR *, LPVOID, DWORD);
    BOOL (CALLBACK *SendExternalMessage)(DWORD, DWORD, DWORD, DWORD, DWORD);
    BOOL (CALLBACK *StartAdvertisingGame)(LPCSTR, LPCSTR, LPCSTR, DWORD, DWORD, DWORD,
                                          DWORD, DWORD, LPCVOID, DWORD);
    BOOL (CALLBACK *StopAdvertisingGame)(void);
    BOOL (CALLBACK *UnlockDeviceList)(LPVOID);
    BOOL (CALLBACK *UnlockGameList)(SNP_GAMELIST_SC *, DWORD *);
    /* --- StarCraft additions; NULL is acceptable for 20-26 --- */
    LPVOID StartAdvertisingGameShort;  /* really GetLocalPlayerName(name, namelen, desc, desclen) */
    LPVOID ReportGameResult;
    LPVOID CheckDataFile;
    LPVOID LeagueCommand;
    LPVOID LeagueSendReplayPath;
    LPVOID LeagueGetReplayPath;
    LPVOID LeagueLogout;
    BOOL (CALLBACK *LeagueGetName)(LPSTR, DWORD);
} SNETSPI_SC, *SNETSPI_SCPTR;

/* ------------------------------------------------------------------ *
 *  Module exports                                 STORM.H:1506
 *
 *  Resolved by name (SNET.CPP:544, 680), so the .def needs no ordinals.
 *  `index` is a 0-based provider index within this module, not an id: storm
 *  walks it upward and stops at the first FALSE.
 * ------------------------------------------------------------------ */

typedef BOOL (APIENTRY *SNETSPIBIND)(DWORD index, void **spi);
typedef BOOL (APIENTRY *SNETSPIQUERY)(DWORD index, DWORD *id, LPCSTR *description,
                                      LPCSTR *requirements, SNETCAPSPTR *caps);

/* ------------------------------------------------------------------ *
 *  Sanity. The exhaustive offset checks live in tests/abi_layout.cpp.
 * ------------------------------------------------------------------ */

#if defined(_M_IX86) || defined(__i386__)
SNP_STATIC_ASSERT(sizeof(SNETCAPS) == 36, "SNETCAPS must be 36 bytes");
SNP_STATIC_ASSERT(sizeof(SNETADDR) == 16, "SNETADDR must be 16 bytes");
SNP_STATIC_ASSERT(sizeof(SNETSPI_GAMELIST) == 308, "Diablo game-list node must be 308 bytes");
SNP_STATIC_ASSERT(sizeof(SNP_GAMELIST_SC) == 316, "StarCraft game-list node must be 316 bytes");
SNP_STATIC_ASSERT(sizeof(SNETSPI_DIABLO) == 84, "Diablo function table must be 84 bytes");
SNP_STATIC_ASSERT(sizeof(SNETSPI_SC) == 112, "StarCraft function table must be 112 bytes");
#endif

#ifdef __cplusplus
}
#endif

#endif /* SNP_STORM_SPI_H */
