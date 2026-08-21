/*
 * Layout assertions for everything storm.dll reads or writes.
 *
 * These are the guardrail behind AGENT.md rule 3. A wrong offset here does not
 * produce a compiler error at the call site -- it produces a crash or a hang
 * inside a 1998 binary with no symbols. Assert the layout instead.
 *
 * This translation unit is compiled, never linked; a build failure is the test
 * failure. See the `abi-check` preset for running it without Windows.
 */

#include "snp/storm_spi.h"

#ifdef _WIN32
#include <cstddef>
#define SNP_OFFSETOF(type, field) offsetof(type, field)
#else
/* Keep the layout check freestanding: no libc headers required. */
#define SNP_OFFSETOF(type, field) __builtin_offsetof(type, field)
#endif

#if !defined(_M_IX86) && !defined(__i386__)
#error "snps targets 32-bit x86. Layout assertions are meaningless elsewhere."
#endif

#define SNP_ASSERT_AT(type, field, offset) \
    static_assert(SNP_OFFSETOF(type, field) == (offset), #type "::" #field " moved")

#define SNP_ASSERT_SIZE(type, bytes) \
    static_assert(sizeof(type) == (bytes), #type " changed size")

/* ---- SNETCAPS -------------------------------------- STORM.H:1217 ---- */

SNP_ASSERT_AT(SNETCAPS, size,                   0);
SNP_ASSERT_AT(SNETCAPS, flags,                  4);
SNP_ASSERT_AT(SNETCAPS, maxmessagesize,         8);
SNP_ASSERT_AT(SNETCAPS, maxqueuesize,          12);
SNP_ASSERT_AT(SNETCAPS, maxplayers,            16);
SNP_ASSERT_AT(SNETCAPS, bytessec,              20);
SNP_ASSERT_AT(SNETCAPS, latencyms,             24);
SNP_ASSERT_AT(SNETCAPS, defaultturnssec,       28);
SNP_ASSERT_AT(SNETCAPS, defaultturnsintransit, 32);
SNP_ASSERT_SIZE(SNETCAPS, 36);

/* ---- Addressing ------------------------------------ STORM.H:1443 ---- */

SNP_ASSERT_SIZE(SNETADDR, 16);

/* ---- Game list, Diablo era ------------------------- STORM.H:1456 ---- */

SNP_ASSERT_AT(SNETSPI_GAMELIST, gameid,            0x000);
SNP_ASSERT_AT(SNETSPI_GAMELIST, gamemode,          0x004);
SNP_ASSERT_AT(SNETSPI_GAMELIST, creationtime,      0x008);
SNP_ASSERT_AT(SNETSPI_GAMELIST, owner,             0x00C);
SNP_ASSERT_AT(SNETSPI_GAMELIST, ownerlatency,      0x01C);
SNP_ASSERT_AT(SNETSPI_GAMELIST, ownerlasttime,     0x020);
SNP_ASSERT_AT(SNETSPI_GAMELIST, gamecategorybits,  0x024);
SNP_ASSERT_AT(SNETSPI_GAMELIST, gamename,          0x028);
SNP_ASSERT_AT(SNETSPI_GAMELIST, gamedescription,   0x0A8);
SNP_ASSERT_AT(SNETSPI_GAMELIST, next,              0x128);
SNP_ASSERT_AT(SNETSPI_GAMELIST, clientdata,        0x12C);
SNP_ASSERT_AT(SNETSPI_GAMELIST, clientdatabytes,   0x130);
SNP_ASSERT_SIZE(SNETSPI_GAMELIST, 308);

/* ---- Game list, StarCraft era -------------------------- [BWAPI] ---- */

SNP_ASSERT_AT(SNP_GAMELIST_SC, clientdatabytes, 0x130);
SNP_ASSERT_AT(SNP_GAMELIST_SC, product,         0x134);
SNP_ASSERT_AT(SNP_GAMELIST_SC, version,         0x138);
SNP_ASSERT_SIZE(SNP_GAMELIST_SC, 316);

/* The StarCraft node is the Diablo node plus two DWORDs. If that stops being
 * true, one of the two transcriptions is wrong. */
static_assert(SNP_OFFSETOF(SNP_GAMELIST_SC, next) == SNP_OFFSETOF(SNETSPI_GAMELIST, next),
              "game-list nodes diverged before the StarCraft additions");
static_assert(sizeof(SNP_GAMELIST_SC) == sizeof(SNETSPI_GAMELIST) + 8,
              "StarCraft game-list node should be the Diablo node plus product+version");

/* ---- Function table, Diablo era -------------------- STORM.H:1472 ---- */

SNP_ASSERT_AT(SNETSPI_DIABLO, size,                0x00);
SNP_ASSERT_AT(SNETSPI_DIABLO, CompareNetAddresses, 0x04);
SNP_ASSERT_AT(SNETSPI_DIABLO, Destroy,             0x08);
SNP_ASSERT_AT(SNETSPI_DIABLO, Free,                0x0C);
SNP_ASSERT_AT(SNETSPI_DIABLO, GetGameInfo,         0x14);
SNP_ASSERT_AT(SNETSPI_DIABLO, Initialize,          0x1C);
SNP_ASSERT_AT(SNETSPI_DIABLO, LockGameList,        0x28);
SNP_ASSERT_AT(SNETSPI_DIABLO, Receive,             0x2C);
SNP_ASSERT_AT(SNETSPI_DIABLO, Send,                0x38);
SNP_ASSERT_AT(SNETSPI_DIABLO, StopAdvertisingGame, 0x44);
SNP_ASSERT_AT(SNETSPI_DIABLO, UnlockGameList,      0x4C);
SNP_ASSERT_AT(SNETSPI_DIABLO, GetLocalPlayerName,  0x50);
SNP_ASSERT_SIZE(SNETSPI_DIABLO, 84);

/* ---- Function table, StarCraft era --------------------- [BWAPI] ---- */

SNP_ASSERT_AT(SNETSPI_SC, size,                      0x00);
SNP_ASSERT_AT(SNETSPI_SC, CompareNetAddresses,       0x04);
SNP_ASSERT_AT(SNETSPI_SC, Receive,                   0x2C);
SNP_ASSERT_AT(SNETSPI_SC, Send,                      0x38);
SNP_ASSERT_AT(SNETSPI_SC, UnlockGameList,            0x4C);
SNP_ASSERT_AT(SNETSPI_SC, StartAdvertisingGameShort, 0x50);
SNP_ASSERT_AT(SNETSPI_SC, LeagueGetName,             0x6C);
SNP_ASSERT_SIZE(SNETSPI_SC, 112);

/* Slots 1-19 must line up across the eras -- that shared prefix is the whole
 * reason one codebase can serve both. Slot 20 is where they part ways. */
#define SNP_ASSERT_SLOT_MATCHES(field) \
    static_assert(SNP_OFFSETOF(SNETSPI_SC, field) == SNP_OFFSETOF(SNETSPI_DIABLO, field), \
                  "slot " #field " differs between eras")

SNP_ASSERT_SLOT_MATCHES(CompareNetAddresses);
SNP_ASSERT_SLOT_MATCHES(Destroy);
SNP_ASSERT_SLOT_MATCHES(Free);
SNP_ASSERT_SLOT_MATCHES(FreeExternalMessage);
SNP_ASSERT_SLOT_MATCHES(GetGameInfo);
SNP_ASSERT_SLOT_MATCHES(GetPerformanceData);
SNP_ASSERT_SLOT_MATCHES(Initialize);
SNP_ASSERT_SLOT_MATCHES(InitializeDevice);
SNP_ASSERT_SLOT_MATCHES(LockDeviceList);
SNP_ASSERT_SLOT_MATCHES(LockGameList);
SNP_ASSERT_SLOT_MATCHES(Receive);
SNP_ASSERT_SLOT_MATCHES(ReceiveExternalMessage);
SNP_ASSERT_SLOT_MATCHES(SelectGame);
SNP_ASSERT_SLOT_MATCHES(Send);
SNP_ASSERT_SLOT_MATCHES(SendExternalMessage);
SNP_ASSERT_SLOT_MATCHES(StartAdvertisingGame);
SNP_ASSERT_SLOT_MATCHES(StopAdvertisingGame);
SNP_ASSERT_SLOT_MATCHES(UnlockDeviceList);
SNP_ASSERT_SLOT_MATCHES(UnlockGameList);

/* Storm rejects any table smaller than this and zero-fills the rest.
 * SNET.CPP:35, 690. */
static_assert(SNP_MINSPISIZE == 80, "MINSPISIZE is 20 * sizeof(DWORD)");
static_assert(SNP_MINSPISIZE == SNP_OFFSETOF(SNETSPI_SC, StartAdvertisingGameShort),
              "the required table ends after UnlockGameList");
static_assert(SNP_MINSPISIZE == SNP_OFFSETOF(SNETSPI_DIABLO, GetLocalPlayerName),
              "the required table ends after UnlockGameList");

/* ---- Initialize descriptors ------------------------ STORM.H:1150 ---- */

SNP_ASSERT_AT(SNETPROGRAMDATA, maxplayers,      24);
SNP_ASSERT_AT(SNETPROGRAMDATA, optcategorybits, 40);
SNP_ASSERT_SIZE(SNETPROGRAMDATA, 60);
SNP_ASSERT_SIZE(SNETPLAYERDATA, 16);
SNP_ASSERT_SIZE(SNETUIDATA, 80);
SNP_ASSERT_SIZE(SNETVERSIONDATA, 20);

/* ---- Constants ------------------------------- STORM.H:32, 41 ------- */

static_assert(SNP_ERROR_NO_MESSAGES_WAITING == 0x8510006Bu,
              "STORMERROR(107) is the 'nothing waiting' code Receive must set");
