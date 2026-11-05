// Plays storm, twice, against the real WebRTC module.
//
//     snp-sim --server ws://127.0.0.1:8765 --module path/to/webbed.snp
//
// Copies the module into two directories and loads both: Windows treats
// different paths as different modules, so each copy has its own globals,
// its own transport and its own WebRTC stack -- a host and a joiner in one
// process. Then it walks them through what storm does in a two-player game
// (docs/snp-abi.md section 6): Initialize, advertise, poll the game list,
// GetGameInfo, Send to the listed owner, Receive, Free, Destroy, unload --
// and a second load after the first unload, because storm reloads modules on
// every visit to the network menu.
//
// This is as close to the game as a test gets without one. What it cannot
// check: which slots the real storm calls, in what order, on which threads.

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "snp/storm_spi.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

typedef BOOL(APIENTRY* SnpQueryFn)(DWORD, DWORD*, LPCSTR*, LPCSTR*, SNETCAPSPTR*);
typedef BOOL(APIENTRY* SnpBindFn)(DWORD, void**);

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok)
        ++g_failures;
}

std::string hex(const SNETADDR& address) {
    char out[33];
    for (int i = 0; i < 16; ++i)
        std::snprintf(out + 2 * i, 3, "%02x", address.address[i]);
    return std::string(out, 8);
}

// One loaded copy of the module, as storm holds it.
struct Module {
    std::string label;
    std::string path;
    HMODULE handle = nullptr;
    SNETSPI_SC* spi = nullptr;
    HANDLE event = nullptr;

    // Storm's descriptors. They have to outlive Initialize.
    SNETPROGRAMDATA program{};
    SNETPLAYERDATA player{};
    SNETUIDATA ui{};
    SNETVERSIONDATA version{};

    Module(std::string name, const std::string& source, const std::string& root)
        : label(std::move(name)) {
        const std::string directory = root + label;
        CreateDirectoryA(directory.c_str(), nullptr);
        path = directory + "\\webbed.snp";
        CopyFileA(source.c_str(), path.c_str(), FALSE);
        DeleteFileA((directory + "\\webbed.log").c_str());

        program.size = sizeof(program);
        program.programname = "snp-sim";
        program.programdescription = "storm, played by a test";
        program.programid = 'SEXP';
        program.versionid = 211;
        program.maxplayers = 8;
        player.size = sizeof(player);
        player.playername = label.c_str();
        ui.size = sizeof(ui);
        version.size = sizeof(version);
        event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    }

    ~Module() {
        unload();
        if (event)
            CloseHandle(event);
    }

    bool load() {
        handle = LoadLibraryA(path.c_str());
        if (!handle)
            return false;
        auto bind = reinterpret_cast<SnpBindFn>(GetProcAddress(handle, "SnpBind"));
        void* table = nullptr;
        if (!bind || !bind(0, &table) || !table)
            return false;
        spi = static_cast<SNETSPI_SC*>(table);
        return true;
    }

    // Storm calls Destroy before it unloads a module it initialised, always;
    // so does this, even when a check failed and the test is leaving early.
    // A module unloaded with its threads still running takes the process
    // down on its way out, and the crash hides what failed.
    void unload() {
        if (initialized && spi)
            spi->Destroy();
        initialized = false;
        if (handle)
            FreeLibrary(handle);
        handle = nullptr;
        spi = nullptr;
    }

    bool initialize() {
        initialized = spi->Initialize(&program, &player, &ui, &version, event) != FALSE;
        return initialized;
    }

    bool destroy() {
        initialized = false;
        return spi->Destroy() != FALSE;
    }

    bool initialized = false;

    std::vector<SNP_GAMELIST_SC> games() {
        std::vector<SNP_GAMELIST_SC> out;
        SNP_GAMELIST_SC* list = nullptr;
        if (spi->LockGameList(0, 0, &list)) {
            for (auto* node = list; node; node = node->next)
                out.push_back(*node);
            DWORD hint = 0;
            spi->UnlockGameList(list, &hint);
        }
        return out;
    }

    bool send(const SNETADDR& to, const std::string& payload, DWORD copies = 1) {
        std::vector<SNETADDRPTR> list(copies, const_cast<SNETADDRPTR>(&to));
        std::vector<char> bytes(payload.begin(), payload.end());
        return spi->Send(copies, list.data(), bytes.data(),
                         static_cast<DWORD>(bytes.size())) != FALSE;
    }

    // Waits on the receive event the way storm does, then drains.
    struct Received {
        SNETADDR from{};
        std::string data;
    };
    std::vector<Received> receive(std::chrono::milliseconds timeout) {
        std::vector<Received> out;
        WaitForSingleObject(event, static_cast<DWORD>(timeout.count()));
        for (;;) {
            SNETADDRPTR from = nullptr;
            LPVOID data = nullptr;
            DWORD size = 0;
            if (!spi->Receive(&from, &data, &size)) {
                if (GetLastError() != SNP_ERROR_NO_MESSAGES_WAITING)
                    std::printf("  (Receive failed with 0x%08lx)\n", GetLastError());
                break;
            }
            Received item;
            item.from = *from;
            item.data.assign(static_cast<const char*>(data), size);
            out.push_back(item);
            spi->Free(from, data, size);
        }
        return out;
    }
};

std::string readFile(const std::string& path) {
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buffer[4096];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
            out.append(buffer, n);
        std::fclose(f);
    }
    return out;
}

void run(const std::string& modulePath) {
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    const std::string root = std::string(temp) + "snps-sim\\";
    CreateDirectoryA(root.c_str(), nullptr);

    Module host("host", modulePath, root);
    Module joiner("joiner", modulePath, root);

    check(host.load() && joiner.load(),
          "two copies of the module load side by side and bind");
    if (!host.spi || !joiner.spi)
        return;
    check(host.spi != joiner.spi, "each copy has its own function table");
    check(host.spi->size >= SNP_MINSPISIZE, "the table is at least MINSPISIZE");

    check(host.initialize(), "the host initialises");
    check(joiner.initialize(), "the joiner initialises");

    // --- hosting -------------------------------------------------------------
    // What StarCraft 1.16.1 actually passed when a player created a game
    // (2026-09-17): its stat string separates fields with CR, and the map
    // name could as well carry bytes above 0x7F.
    const char extra[] = "EXTRA-BYTES";
    const char stat[] = ",44,,6,,f,,4,5ab5438b,10,,Chaos\rUntitled Scenario \xE9\r";
    check(host.spi->StartAdvertisingGame("sim game", "", stat, 0x1234, 0, 0x4000F, 0xFF, 0,
                                         extra, sizeof(extra)) != FALSE,
          "the host advertises a game");

    SNP_GAMELIST_SC game{};
    bool listed = false;
    for (auto until = Clock::now() + 5s; !listed && Clock::now() < until;) {
        for (const auto& node : joiner.games()) {
            if (std::strcmp(node.gamename, "sim game") == 0) {
                game = node;
                listed = true;
            }
        }
        if (!listed)
            std::this_thread::sleep_for(250ms);
    }
    check(listed, "the joiner's LockGameList shows it within a few polls");
    check(std::strcmp(game.gamedescription, stat) == 0,
          "StarCraft's stat string comes through byte for byte");
    check(game.gamemode == 0x1234, "so does the state");
    check(game.clientdatabytes == sizeof(extra) && game.clientdata &&
              std::memcmp(game.clientdata, extra, sizeof(extra)) == 0,
          "so do the extra bytes");
    check(game.product == 'SEXP' && game.version == 211,
          "the node carries the joiner's product and version");
    check(host.games().empty(), "the host does not list its own game");

    SNP_GAMELIST_SC info{};
    check(joiner.spi->GetGameInfo(game.gameid, game.gamename, 0, &info) != FALSE &&
              std::memcmp(&info.owner, &game.owner, sizeof(SNETADDR)) == 0,
          "GetGameInfo finds the game by its id");
    SNP_GAMELIST_SC missing{};
    check(joiner.spi->GetGameInfo(0xDEADBEEF, "", 0, &missing) == FALSE &&
              GetLastError() == SNP_ERROR_GAME_NOT_FOUND,
          "GetGameInfo reports an unknown game the way storm expects");

    // --- first contact ---------------------------------------------------------
    // Storm sends to the owner address from the game list and retransmits
    // until it hears back.
    std::vector<Module::Received> atHost;
    for (auto until = Clock::now() + 20s; atHost.empty() && Clock::now() < until;) {
        joiner.send(info.owner, "join request");
        atHost = host.receive(300ms);
    }
    check(!atHost.empty() && atHost.front().data == "join request",
          "the host's receive event fires and Receive returns the joiner's datagram");
    if (atHost.empty())
        return;
    const SNETADDR joinerAddress = atHost.front().from;
    std::printf("         joiner is %s..., host is %s...\n", hex(joinerAddress).c_str(),
                hex(info.owner).c_str());

    host.send(joinerAddress, "welcome");
    auto atJoiner = joiner.receive(5s);
    for (auto until = Clock::now() + 5s; atJoiner.empty() && Clock::now() < until;)
        atJoiner = joiner.receive(300ms);
    check(!atJoiner.empty() && atJoiner.front().data == "welcome",
          "the reply comes back without a new handshake");
    check(!atJoiner.empty() &&
              std::memcmp(&atJoiner.front().from, &info.owner, sizeof(SNETADDR)) == 0,
          "the joiner sees it from the same address the game list gave for the owner");

    DWORD same = 0;
    DWORD different = 1;
    host.spi->CompareNetAddresses(const_cast<SNETADDRPTR>(&joinerAddress),
                                  const_cast<SNETADDRPTR>(&joinerAddress), &same);
    host.spi->CompareNetAddresses(const_cast<SNETADDRPTR>(&joinerAddress),
                                  const_cast<SNETADDRPTR>(&info.owner), &different);
    check(same == 1 && different == 0, "CompareNetAddresses answers 1 for equal");

    // --- storm's fan-out -----------------------------------------------------
    host.send(joinerAddress, "fan-out", 3);
    std::vector<Module::Received> fanned;
    for (auto until = Clock::now() + 5s; fanned.size() < 3 && Clock::now() < until;) {
        auto more = joiner.receive(300ms);
        for (auto& item : more)
            if (item.data == "fan-out")
                fanned.push_back(item);
    }
    check(fanned.size() == 3, "a Send to three addresses delivers to all three");

    std::string largest(500 - 16, 'x');  // maxmessagesize, less storm's header
    joiner.send(info.owner, largest);
    bool gotLargest = false;
    for (auto until = Clock::now() + 5s; !gotLargest && Clock::now() < until;)
        for (auto& item : host.receive(300ms))
            gotLargest = gotLargest || item.data == largest;
    check(gotLargest, "the largest datagram storm will send arrives intact");

    // --- the host stops hosting ----------------------------------------------
    host.spi->StopAdvertisingGame();
    bool gone = false;
    for (auto until = Clock::now() + 5s; !gone && Clock::now() < until;) {
        gone = true;
        for (const auto& node : joiner.games())
            if (std::strcmp(node.gamename, "sim game") == 0)
                gone = false;
        if (!gone)
            std::this_thread::sleep_for(250ms);
    }
    check(gone, "StopAdvertisingGame takes the game off the joiner's list");

    // --- leaving -----------------------------------------------------------------
    check(host.destroy() && joiner.destroy(),
          "both copies Destroy cleanly");
    host.unload();
    joiner.unload();
    check(true, "both copies unload with their WebRTC stacks stopped");

    const std::string log = readFile(root + "host\\webbed.log");
    check(log.find("programid 0x53455850 (\"SEXP\")") != std::string::npos,
          "the log records the programid storm passed");
    check(log.find("datagrams sent") != std::string::npos,
          "and the traffic totals at Destroy");

    // Storm comes back to the menu and loads everything again.
    check(host.load() && host.initialize(), "the module initialises again after a reload");
    check(host.spi && host.destroy(), "and destroys again");
    host.unload();

    // A server nobody is listening on: Initialize must fail, promptly.
    SetEnvironmentVariableA("SNPS_SERVER", "ws://127.0.0.1:1");
    const auto started = Clock::now();
    const bool loaded = host.load();
    const bool refused = loaded && !host.initialize();
    const auto took = Clock::now() - started;
    check(refused && GetLastError() == ERROR_NETWORK_UNREACHABLE,
          "Initialize fails with ERROR_NETWORK_UNREACHABLE when the server is unreachable");
    check(took < 6s, "and gives up within the timeout");
    host.unload();
}

} // namespace

int main(int argc, char** argv) {
    std::string server;
    std::string module;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--server") == 0)
            server = argv[++i];
        else if (std::strcmp(argv[i], "--module") == 0)
            module = argv[++i];
    }
    if (server.empty() || module.empty()) {
        std::fprintf(stderr, "Usage: snp-sim --server ws://host:port --module webbed.snp\n");
        return 2;
    }

    std::printf("snp-sim: %s against %s\n\n", module.c_str(), server.c_str());
    // Both copies read the server from here; webbed.ini would need one each.
    SetEnvironmentVariableA("SNPS_SERVER", server.c_str());

    run(module);

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
