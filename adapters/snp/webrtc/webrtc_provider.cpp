/*
 * The WebRTC provider: storm's function table over snps::Transport.
 *
 * Storm does sequencing, acknowledgement and retransmission itself, so every
 * datagram goes over the unreliable channel; layering reliability underneath
 * would wreck the round-trip estimates its timeouts are built on (AGENT.md
 * section 4). The game list is the signalling server's directory, and a
 * player's SNETADDR is its 16-byte peer id -- globally stable, which is what
 * lets storm gossip addresses and then send to strangers (docs/snp-abi.md
 * section 6).
 *
 * Behaviour copied from BWAPI's DirectIP, the only working 1.16.1 module to
 * read, is marked [W]. Nothing is copied from its code.
 *
 * Configuration, first match wins:
 *   - the SNPS_SERVER environment variable
 *   - webbed.ini beside the module:   [snps]  server=ws://host:port
 *   - ws://127.0.0.1:8765
 * The same file takes  log=off|error|warning|info|debug  (default info); the
 * log is webbed.log beside the module, or in %TEMP% if that directory is not
 * writable. Both take the module's own name: a copy renamed x.snp reads x.ini.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "snp/storm_spi.h"
#include "snps/transport.hpp"

#include "webrtc_caps.h"

namespace {

using Clock = std::chrono::steady_clock;

const DWORD kProviderCount =
    static_cast<DWORD>(sizeof(snp_caps_records) / sizeof(snp_caps_records[0]));

// How long Initialize waits for the signalling server before telling storm
// the network is unavailable. Storm's menu is frozen for that long.
constexpr auto kOnlineTimeout = std::chrono::seconds(4);
// Storm polls the game list about once a second [W]; asking the server more
// often than that buys nothing.
constexpr auto kRefreshInterval = std::chrono::milliseconds(900);

HINSTANCE g_module = nullptr;

// ---- Files beside the module -----------------------------------------------

// The module's path without its extension. Its settings and its log take the
// same name beside it: webbed.snp reads webbed.ini and writes webbed.log.
std::string moduleStem() {
    char path[MAX_PATH] = "";
    DWORD length = GetModuleFileNameA(g_module, path, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return "webbed";
    std::string stem(path, length);
    const auto slash = stem.find_last_of("\\/");
    const auto dot = stem.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        stem.erase(dot);
    return stem;
}

std::string iniValue(const char* key, const char* fallback) {
    const std::string ini = moduleStem() + ".ini";
    char value[512] = "";
    GetPrivateProfileStringA("snps", key, fallback, value, sizeof(value), ini.c_str());
    return value;
}

// ---- Logging ------------------------------------------------------------------

std::mutex g_logMutex;
FILE* g_log = nullptr;
snps::LogLevel g_logLevel = snps::LogLevel::Info;
bool g_logEnabled = true;

const char* levelName(snps::LogLevel level) {
    switch (level) {
    case snps::LogLevel::Debug: return "debug";
    case snps::LogLevel::Info: return "info ";
    case snps::LogLevel::Warning: return "warn ";
    default: return "error";
    }
}

void openLog() {
    const std::string setting = iniValue("log", "info");
    g_logEnabled = setting != "off";
    if (setting == "debug") g_logLevel = snps::LogLevel::Debug;
    else if (setting == "warning") g_logLevel = snps::LogLevel::Warning;
    else if (setting == "error") g_logLevel = snps::LogLevel::Error;
    else g_logLevel = snps::LogLevel::Info;
    if (!g_logEnabled)
        return;

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log)
        return;
    const std::string stem = moduleStem();
    g_log = std::fopen((stem + ".log").c_str(), "a");
    if (!g_log) {
        char temp[MAX_PATH] = "";
        const auto slash = stem.find_last_of("\\/");
        const std::string name = slash == std::string::npos ? stem : stem.substr(slash + 1);
        if (GetTempPathA(MAX_PATH, temp))
            g_log = std::fopen((std::string(temp) + name + ".log").c_str(), "a");
    }
}

void closeLog() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log) {
        std::fclose(g_log);
        g_log = nullptr;
    }
}

void write(snps::LogLevel level, const std::string& line) {
    if (!g_logEnabled || level < g_logLevel)
        return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    // The process id tells apart two games run from one folder, which share
    // this module and so this log.
    char stamp[48];
    std::snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u [%lu]", now.wHour, now.wMinute,
                  now.wSecond, now.wMilliseconds, GetCurrentProcessId());

    const std::string text = std::string(stamp) + " " + levelName(level) + " " + line;
    OutputDebugStringA(("snps: " + text + "\n").c_str());

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log) {
        std::fprintf(g_log, "%s\n", text.c_str());
        std::fflush(g_log);
    }
}

void logf(snps::LogLevel level, const char* format, ...) {
    if (!g_logEnabled || level < g_logLevel)
        return;
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    write(level, buffer);
}

// ---- State ----------------------------------------------------------------------

// A game list entry in storm's layout, with the storage its pointers need.
struct Listing {
    SNP_GAMELIST_SC node{};
    std::vector<std::uint8_t> extra;
    std::string room;
};

std::mutex g_mutex;                            // everything below except the list
std::unique_ptr<snps::Transport> g_transport;
HANDLE g_receiveEvent = nullptr;
DWORD g_programId = 0;
DWORD g_versionId = 0;
DWORD g_maxPlayers = 8;
Clock::time_point g_lastRefresh{};
std::vector<std::pair<std::string, DWORD>> g_gameIds;  // room id -> storm's game id
DWORD g_nextGameId = 1;

// Experiments on the game-list entry, read from webbed.ini at Initialize. The
// game hides our listings for a reason nobody has found yet, and each switch
// moves one field toward what BWAPI's Local PC hands it -- whose listings it
// shows. See adapters/snp/webrtc/README.md.
struct ListingOptions {
    DWORD firstId = 1;        // list_first_id    Local PC's end up 0
    int extra = 0;            // list_extra       pass | zero | none
    DWORD category = 0xA7;    // list_category    DirectIP's constant
    DWORD latency = 0x50;     // list_latency     DirectIP's constant
    bool showOwn = false;     // list_show_own    Local PC lists the host's own game
    bool zeroOwner = false;   // list_owner=zero  breaks joining; for listing only
};
ListingOptions g_listing;

DWORD iniNumber(const char* key, DWORD fallback) {
    const std::string text = iniValue(key, "");
    if (text.empty())
        return fallback;
    return static_cast<DWORD>(std::strtoul(text.c_str(), nullptr, 0));
}

void readListingOptions() {
    g_listing = ListingOptions{};
    g_listing.firstId = iniNumber("list_first_id", g_listing.firstId);
    const std::string extra = iniValue("list_extra", "pass");
    g_listing.extra = extra == "zero" ? 1 : extra == "none" ? 2 : 0;
    g_listing.category = iniNumber("list_category", g_listing.category);
    g_listing.latency = iniNumber("list_latency", g_listing.latency);
    g_listing.showOwn = iniNumber("list_show_own", 0) != 0;
    g_listing.zeroOwner = iniValue("list_owner", "peer") == "zero";
}

// The list storm reads is held from LockGameList to UnlockGameList, the way
// DirectIP holds it [W] -- storm walks the linked nodes in between. A
// CRITICAL_SECTION because the lock is released by a later call on the same
// thread, which std::mutex does not promise to allow either.
CRITICAL_SECTION g_listLock;
std::vector<std::unique_ptr<Listing>> g_listings;
std::string g_lastListed;  // for logging changes only

std::atomic<std::uint64_t> g_sent{0};
std::atomic<std::uint64_t> g_received{0};
std::atomic<std::uint64_t> g_listCalls{0};

// Storm's addresses are our peer ids, byte for byte.
snps::PeerId toPeer(const SNETADDR* address) {
    snps::PeerId id{};
    std::memcpy(id.data(), address, id.size());
    return id;
}

// The FOURCC storm passes as programid, e.g. 'SEXP', as the four characters
// the signalling server scopes its directory by.
std::string productCode(DWORD programId) {
    std::string code;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char c = static_cast<char>((programId >> shift) & 0xFF);
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return {};
        code += c;
    }
    return code;
}

std::string hexBytes(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    char pair[4];
    for (const auto b : bytes) {
        std::snprintf(pair, sizeof(pair), out.empty() ? "%02x" : " %02x", b);
        out += pair;
    }
    return out;
}

DWORD gameIdFor(const std::string& room) {
    for (const auto& entry : g_gameIds)
        if (entry.first == room)
            return entry.second;
    g_gameIds.emplace_back(room, g_nextGameId);
    return g_nextGameId++;
}

void copyString(char* destination, std::size_t size, const std::string& source) {
    const std::size_t count = std::min(size - 1, source.size());
    std::memcpy(destination, source.data(), count);
    destination[count] = '\0';
}

// ---- The packet storm borrows between Receive and Free -------------------------

// The address comes first, because Free is handed the address pointer and has
// to find the whole allocation from it (docs/snp-abi.md section 6).
struct Packet {
    SNETADDR address;
    DWORD size;
    BYTE data[1];
};

// ---- The function table -----------------------------------------------------

BOOL CALLBACK CompareNetAddresses(SNETADDRPTR a, SNETADDRPTR b, DWORD* result) {
    if (result)
        *result = 0;
    if (!a || !b || !result) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    // 1 means equal, whatever the parameter's name suggests (NULL.CPP:31) [S].
    *result = std::memcmp(a, b, sizeof(SNETADDR)) == 0;
    return TRUE;
}

BOOL CALLBACK Destroy(void) {
    std::unique_ptr<snps::Transport> transport;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        transport = std::move(g_transport);
        g_receiveEvent = nullptr;
        g_gameIds.clear();
    }
    if (transport) {
        logf(snps::LogLevel::Info,
             "destroy: %llu datagrams sent, %llu received, %llu dropped; "
             "LockGameList called %llu times",
             static_cast<unsigned long long>(g_sent.load()),
             static_cast<unsigned long long>(g_received.load()),
             static_cast<unsigned long long>(transport->dropped()),
             static_cast<unsigned long long>(g_listCalls.load()));
        transport->withdraw();
        transport.reset();
        snps::shutdown();
    }
    EnterCriticalSection(&g_listLock);
    g_listings.clear();
    g_lastListed.clear();
    LeaveCriticalSection(&g_listLock);

    snps::setLogSink(nullptr);
    closeLog();
    return TRUE;
}

BOOL CALLBACK Free(SNETADDRPTR address, LPVOID data, DWORD) {
    if (!address || !data) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ::operator delete(reinterpret_cast<Packet*>(address));
    return TRUE;
}

BOOL CALLBACK FreeExternalMessage(SNETADDRPTR, LPVOID, DWORD) {
    SetLastError(SNP_ERROR_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL CALLBACK GetGameInfo(DWORD index, LPCSTR name, DWORD, SNP_GAMELIST_SC* result) {
    if (!result) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    std::string room;
    EnterCriticalSection(&g_listLock);
    for (const auto& listing : g_listings) {
        // DirectIP matches on the index alone [W]; the name is a fallback.
        if (listing->node.gameid == index ||
            (name && *name && std::strcmp(listing->node.gamename, name) == 0)) {
            *result = listing->node;
            result->next = nullptr;
            room = listing->room;
            break;
        }
    }
    LeaveCriticalSection(&g_listLock);

    if (room.empty()) {
        logf(snps::LogLevel::Warning, "GetGameInfo: no game %lu (%s)", index, name ? name : "");
        SetLastError(SNP_ERROR_GAME_NOT_FOUND);
        return FALSE;
    }

    // Storm is about to talk to the host. Joining the room is what lets the
    // server relay our introduction to it.
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_transport) {
        logf(snps::LogLevel::Info, "joining game %s (\"%s\")", room.c_str(), result->gamename);
        g_transport->join(room);
    }
    return TRUE;
}

BOOL CALLBACK GetPerformanceData(DWORD, DWORD*, DWORD, DWORD) {
    SetLastError(SNP_ERROR_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL CALLBACK Initialize(SNETPROGRAMDATAPTR program, SNETPLAYERDATAPTR player,
                         SNETUIDATAPTR, SNETVERSIONDATAPTR, HANDLE receiveEvent) {
    openLog();
    snps::setLogSink([](snps::LogLevel level, const std::string& line) { write(level, line); });

    if (!program || !receiveEvent) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    std::string server;
    char env[512] = "";
    if (GetEnvironmentVariableA("SNPS_SERVER", env, sizeof(env)) && *env)
        server = env;
    else
        server = iniValue("server", "ws://127.0.0.1:8765");

    readListingOptions();
    logf(snps::LogLevel::Info,
         "listing: first id %lu, extra %s, category 0x%lx, latency 0x%lx, own game %s, "
         "owner %s",
         g_listing.firstId,
         g_listing.extra == 1 ? "zeroed" : g_listing.extra == 2 ? "none" : "passed",
         g_listing.category, g_listing.latency, g_listing.showOwn ? "shown" : "hidden",
         g_listing.zeroOwner ? "zeroed" : "peer id");

    const std::string product = productCode(program->programid);
    const std::string name = player && player->playername ? player->playername : "";

    // Which program loaded us: the game's own executable, or something that
    // hosts it, like a launcher or a reimplementation. It matters when the
    // game misbehaves.
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* program_file = std::strrchr(exe, '\\') ? std::strrchr(exe, '\\') + 1 : exe;

    // programid's values are [?] in docs/snp-abi.md section 7; this line is
    // how they get confirmed.
    logf(snps::LogLevel::Info,
         "initialize: %s, programid 0x%08lx (\"%s\"), versionid %lu, maxplayers %lu, "
         "programdata %lu bytes, player \"%s\", server %s, process %s",
         snp_caps_records[0].description, program->programid, product.c_str(),
         program->versionid, program->maxplayers, program->size, name.c_str(),
         server.c_str(), program_file);

    snps::Options options;
    options.server = server;
    options.product = product.empty() ? "SNPS" : product;
    options.version = static_cast<int>(program->versionid);
    options.name = name;

    auto transport = std::make_unique<snps::Transport>(options);
    transport->onReceivable([receiveEvent] { SetEvent(receiveEvent); });

    snps::initialize();
    transport->start();
    if (!transport->waitOnline(kOnlineTimeout)) {
        logf(snps::LogLevel::Error, "initialize: cannot reach %s: %s", server.c_str(),
             transport->error().empty() ? "timed out" : transport->error().c_str());
        transport.reset();
        snps::shutdown();
        snps::setLogSink(nullptr);
        closeLog();
        SetLastError(ERROR_NETWORK_UNREACHABLE);
        return FALSE;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_transport = std::move(transport);
    g_receiveEvent = receiveEvent;
    g_programId = program->programid;
    g_versionId = program->versionid;
    g_maxPlayers = program->maxplayers ? program->maxplayers : 8;
    g_lastRefresh = Clock::time_point{};
    g_gameIds.clear();
    g_nextGameId = g_listing.firstId;
    g_sent = 0;
    g_received = 0;
    g_listCalls = 0;
    g_transport->refreshGames();
    return TRUE;
}

BOOL CALLBACK InitializeDevice(DWORD, LPVOID, LPVOID, DWORD*, LPVOID) {
    SetLastError(SNP_ERROR_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL CALLBACK LockDeviceList(DWORD* list) {
    if (list)
        *list = 0;  // no devices; storm stops asking [W]
    return TRUE;
}

BOOL CALLBACK LockGameList(DWORD categoryBits, DWORD categoryMask, SNP_GAMELIST_SC** list) {
    if (!list) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    // What the game filters on is not known; what it asks for is worth a line.
    static DWORD s_lastBits = 0xFFFFFFFF;
    static DWORD s_lastMask = 0xFFFFFFFF;
    ++g_listCalls;
    if (categoryBits != s_lastBits || categoryMask != s_lastMask) {
        logf(snps::LogLevel::Info, "LockGameList asks for category bits 0x%08lx, mask 0x%08lx",
             categoryBits, categoryMask);
        s_lastBits = categoryBits;
        s_lastMask = categoryMask;
    }

    std::vector<snps::Game> games;
    std::optional<snps::PeerId> self;
    std::vector<std::pair<std::string, DWORD>> ids;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_transport) {
            const auto now = Clock::now();
            if (now - g_lastRefresh >= kRefreshInterval) {
                g_lastRefresh = now;
                g_transport->refreshGames();  // the answer shows up next time
            }
            games = g_transport->games();
            self = g_transport->self();
            for (const auto& game : games)
                ids.emplace_back(game.id, gameIdFor(game.id));
        }
    }

    // Held until UnlockGameList.
    EnterCriticalSection(&g_listLock);

    std::vector<std::unique_ptr<Listing>> fresh;
    const DWORD now = GetTickCount();
    for (std::size_t i = 0; i < games.size(); ++i) {
        const auto& game = games[i];
        if (self && game.host == *self && !g_listing.showOwn)
            continue;  // our own game

        auto listing = std::make_unique<Listing>();
        listing->room = game.id;
        listing->extra = game.extra;
        if (g_listing.extra == 1)
            std::fill(listing->extra.begin(), listing->extra.end(), std::uint8_t{0});
        else if (g_listing.extra == 2)
            listing->extra.clear();
        SNP_GAMELIST_SC& node = listing->node;
        node.gameid = ids[i].second;
        node.gamemode = game.state;
        node.creationtime = 0;
        if (!g_listing.zeroOwner)
            std::memcpy(&node.owner, game.host.data(), sizeof(SNETADDR));
        // DirectIP advertises these two constants and its games are listed
        // and joinable [W]. The host's real category bits would be the honest
        // value (storm_spi.h), but they do not travel through the server yet.
        node.ownerlatency = g_listing.latency;
        node.ownerlasttime = now;
        node.gamecategorybits = g_listing.category;
        copyString(node.gamename, sizeof(node.gamename), game.name);
        copyString(node.gamedescription, sizeof(node.gamedescription), game.stat);
        node.clientdata = listing->extra.empty() ? nullptr : listing->extra.data();
        node.clientdatabytes = static_cast<DWORD>(listing->extra.size());
        node.product = g_programId;
        node.version = g_versionId;
        fresh.push_back(std::move(listing));
    }
    for (std::size_t i = 0; i + 1 < fresh.size(); ++i)
        fresh[i]->node.next = &fresh[i + 1]->node;

    // Logged when it changes, so a game that is in this list but missing from
    // the game's own screen can be told apart from one that never arrived --
    // the game filters what storm hands it, and says nothing when it does.
    std::string names;
    for (const auto& listing : fresh) {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "\" (id %lu, mode 0x%lx, extra [",
                      listing->node.gameid, listing->node.gamemode);
        names += (names.empty() ? "\"" : ", \"") + std::string(listing->node.gamename) +
                 detail + hexBytes(listing->extra) + "])";
    }
    if (names != g_lastListed) {
        logf(snps::LogLevel::Info, "game list now holds %u game(s)%s%s",
             static_cast<unsigned>(fresh.size()), names.empty() ? "" : ": ", names.c_str());
        g_lastListed = names;
    }

    g_listings = std::move(fresh);
    *list = g_listings.empty() ? nullptr : &g_listings.front()->node;
    return TRUE;
}

BOOL CALLBACK Receive(SNETADDRPTR* address, LPVOID* data, DWORD* size) {
    if (address) *address = nullptr;
    if (data) *data = nullptr;
    if (size) *size = 0;
    if (!address || !data || !size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    snps::Datagram datagram;
    bool got = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // Storm's traffic rides the unreliable channel. A datagram that came
        // through the signalling server is not storm's -- this module is not
        // open, so only a co-member could have sent one -- and is dropped.
        while (!got && g_transport && g_transport->receive(datagram))
            got = datagram.channel != snps::Channel::Server;
    }
    if (!got) {
        // Not a bare FALSE: storm tells "nothing yet" from failure by this.
        SetLastError(SNP_ERROR_NO_MESSAGES_WAITING);
        return FALSE;
    }

    const std::size_t bytes = offsetof(Packet, data) + datagram.data.size();
    auto* packet = static_cast<Packet*>(::operator new(bytes));
    std::memcpy(&packet->address, datagram.from.data(), sizeof(SNETADDR));
    packet->size = static_cast<DWORD>(datagram.data.size());
    std::memcpy(packet->data, datagram.data.data(), datagram.data.size());
    ++g_received;

    *address = &packet->address;
    *data = packet->data;
    *size = packet->size;
    return TRUE;
}

BOOL CALLBACK ReceiveExternalMessage(SNETADDRPTR* address, LPVOID* data, DWORD* size) {
    if (address) *address = nullptr;
    if (data) *data = nullptr;
    if (size) *size = 0;
    SetLastError(SNP_ERROR_NO_MESSAGES_WAITING);
    return FALSE;
}

BOOL CALLBACK SelectGame(DWORD, SNETPROGRAMDATAPTR, SNETPLAYERDATAPTR, SNETUIDATAPTR,
                         SNETVERSIONDATAPTR, DWORD*) {
    SetLastError(SNP_ERROR_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL CALLBACK Send(DWORD count, SNETADDRPTR* addresses, LPVOID data, DWORD size) {
    if (!count)
        return TRUE;
    if (!addresses || !data || !size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_transport) {
        SetLastError(ERROR_NOT_READY);
        return FALSE;
    }
    // Every address, not just the first: storm sends to up to 16 at once
    // (SNET.CPP:856) [S], and the most-copied public module drops the rest.
    const DWORD limit = (std::min)(count, static_cast<DWORD>(16));
    for (DWORD i = 0; i < limit; ++i) {
        if (!addresses[i])
            continue;
        g_transport->send(toPeer(addresses[i]), snps::Channel::Unreliable,
                          static_cast<const std::uint8_t*>(data), size);
        ++g_sent;
    }
    return TRUE;
}

BOOL CALLBACK SendExternalMessage(DWORD, DWORD, DWORD, DWORD, DWORD) {
    SetLastError(SNP_ERROR_NOT_IMPLEMENTED);
    return FALSE;
}

// Ten parameters in the StarCraft era. Storm's own call (SNET.CPP:3067-3082)
// names the sixth and seventh: game category bits and the program's optional
// category bits -- which StarCraft sets to 0xFF, as its log and its
// SNETPROGRAMDATA both show. The eighth is StarCraft's addition and still
// UNVERIFIED. The extra bytes are storm's CLIENTDATA {bytes, numplayers,
// maxplayers} (SNET.CPP:67, 1456).
BOOL CALLBACK StartAdvertisingGame(LPCSTR name, LPCSTR password, LPCSTR stat, DWORD state,
                                   DWORD elapsed, DWORD categoryBits, DWORD optCategoryBits,
                                   DWORD unknown8, LPCVOID extra, DWORD extraBytes) {
    if (!name) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    snps::Advert advert;
    advert.name = name;
    advert.stat = stat ? stat : "";
    advert.state = state;
    if (extra && extraBytes) {
        const auto* bytes = static_cast<const std::uint8_t*>(extra);
        // The server keeps 32 bytes, as DirectIP does [W].
        advert.extra.assign(bytes, bytes + (std::min)(extraBytes, static_cast<DWORD>(32)));
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_transport) {
        SetLastError(ERROR_NOT_READY);
        return FALSE;
    }
    advert.maxPlayers = static_cast<int>((std::min)(g_maxPlayers, static_cast<DWORD>(16)));

    logf(snps::LogLevel::Info,
         "advertising \"%s\": password %s, state 0x%lx, elapsed %lu, category 0x%lx, "
         "optional category 0x%lx, p8 0x%lx, extra [%s], stat \"%s\"",
         name, password && *password ? "set" : "none", state, elapsed, categoryBits,
         optCategoryBits, unknown8, hexBytes(advert.extra).c_str(), advert.stat.c_str());
    g_transport->advertise(advert);
    return TRUE;
}

BOOL CALLBACK StopAdvertisingGame(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_transport) {
        logf(snps::LogLevel::Info, "no longer advertising");
        g_transport->withdraw();
    }
    return TRUE;
}

BOOL CALLBACK UnlockDeviceList(LPVOID) { return TRUE; }

BOOL CALLBACK UnlockGameList(SNP_GAMELIST_SC*, DWORD*) {
    LeaveCriticalSection(&g_listLock);
    return TRUE;
}

SNETSPI_SC g_spi = {
    static_cast<DWORD>(SNP_MINSPISIZE),
    CompareNetAddresses,
    Destroy,
    Free,
    FreeExternalMessage,
    GetGameInfo,
    GetPerformanceData,
    Initialize,
    InitializeDevice,
    LockDeviceList,
    LockGameList,
    Receive,
    ReceiveExternalMessage,
    SelectGame,
    Send,
    SendExternalMessage,
    StartAdvertisingGame,
    StopAdvertisingGame,
    UnlockDeviceList,
    UnlockGameList,
    /* slots 20-27: NULL, as DirectIP leaves most of them [W] */
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
};

}  // namespace

extern "C" BOOL APIENTRY SnpQuery(DWORD index, DWORD* id, LPCSTR* description,
                                  LPCSTR* requirements, SNETCAPSPTR* caps) {
    if (!id || !description || !requirements || !caps)
        return FALSE;
    if (index >= kProviderCount)
        return FALSE;
    const SNP_CAPS_RECORD& record = snp_caps_records[index];
    *id = record.id;
    *description = record.description;
    *requirements = record.requirements;
    *caps = const_cast<SNETCAPSPTR>(&record.caps);
    return TRUE;
}

extern "C" BOOL APIENTRY SnpBind(DWORD index, void** spi) {
    if (!spi || index >= kProviderCount)
        return FALSE;
    *spi = &g_spi;
    return TRUE;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    // Storm loads every module just to enumerate it, so nothing here may do
    // more than record the handle and make a lock usable. The WebRTC stack
    // starts in Initialize and stops in Destroy.
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_listLock);
    } else if (reason == DLL_PROCESS_DETACH) {
        DeleteCriticalSection(&g_listLock);
    }
    return TRUE;
}
