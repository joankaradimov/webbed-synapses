/*
 * The null provider.
 *
 * It carries no traffic. Its job is to be the smallest thing storm will load,
 * so that "does our module get enumerated, bound, and initialized by a real
 * game?" can be answered before any transport code exists. Blizzard shipped
 * one for the same reason (STANDARD/NULL.CPP).
 *
 * It reports a table of exactly SNP_MINSPISIZE bytes: the size field plus the
 * nineteen slots storm requires. Storm zero-fills the rest (SNET.CPP:696), so
 * this one module is shaped correctly for both eras -- with the caveat in
 * AGENT.md section 4 that slot 11's calling contract still differs, which
 * costs nothing here because we never return a packet.
 */

#include <windows.h>
#include <string.h>

#include "snp/storm_spi.h"

/* The provider's id, strings and capabilities live here and nowhere else; the
 * build compiles the same header into the tool that writes caps.dat, so the
 * archive and the module cannot disagree. See snp/caps_record.h. */
#include "null_caps.h"

namespace {

/* ---- Breadcrumbs -------------------------------------------------------
 *
 * Storm reports nothing when it rejects a module, and there are two separate
 * places it can happen: it skips the file outright if the appended archive
 * will not open or authenticate, and it drops the provider from the menu list
 * later if the caps fail SpiMeetsMinimumCaps against what the game asks for
 * (SNET.CPP:530, 2853-2861). The two look identical from the outside.
 *
 * This tells them apart, because storm only reaches LoadLibrary once the
 * archive has been accepted (SNET.CPP:526-536):
 *
 *   no log file at all      the archive was rejected -- signature or format
 *   "attach" but no query   the exports were not found
 *   "SnpQuery 0 -> 1"       the module is enumerated; if it is still not in
 *                           the menu, the caps filter is hiding it
 *   "Initialize"            the player picked it and storm bound to it
 *
 * The log lands next to the .snp, and everything also goes to the debugger via
 * OutputDebugString for when that directory is not writable. Doing file I/O
 * from DllMain is not something to imitate in a real module -- it happens
 * under the loader lock -- but it touches nothing but kernel32 and this module
 * exists to be diagnosed.
 */

HINSTANCE g_module = nullptr;

int append(char* line, int at, const char* text, int limit) {
    for (const char* p = text; *p && at < limit; ++p)
        line[at++] = *p;
    return at;
}

/* The log goes beside the module, or into %TEMP% when that directory is not
 * writable -- a game in Program Files run without elevation cannot write to
 * its own directory, and a failed write is indistinguishable from never having
 * been loaded, which is the one distinction this whole mechanism exists to
 * make. Windows may also silently redirect the first write into
 * %LOCALAPPDATA%\VirtualStore\..., so look there too.
 *
 * OutputDebugString needs no filesystem at all: DebugView shows these lines
 * whatever the permissions are. */
bool write_to(const char* directory, int length, const char* line, int bytes) {
    char path[MAX_PATH];
    if (length <= 0 || length + 16 >= MAX_PATH) return false;
    for (int i = 0; i < length; ++i) path[i] = directory[i];
    length = append(path, length, "snp-null.log", MAX_PATH - 1);
    path[length] = '\0';

    HANDLE file = CreateFileA(path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, line, static_cast<DWORD>(bytes), &written,
                              nullptr);
    CloseHandle(file);
    return ok != FALSE;
}

void trace(const char* what, const char* extra, DWORD value, bool has_value) {
    char line[512];
    int at = append(line, 0, what, 400);
    if (extra)
        at = append(line, at, extra, 460);
    if (has_value) {
        line[at++] = ' ';
        char digits[16];
        int count = 0;
        do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; }
        while (value && count < 15);
        while (count) line[at++] = digits[--count];
    }
    line[at] = '\0';
    OutputDebugStringA(line);

    line[at++] = '\r';
    line[at++] = '\n';

    char directory[MAX_PATH];
    DWORD length = GetModuleFileNameA(g_module, directory, MAX_PATH);
    if (length && length < MAX_PATH) {
        while (length && directory[length - 1] != '\\' &&
               directory[length - 1] != '/')
            --length;
        if (write_to(directory, static_cast<int>(length), line, at))
            return;
    }

    length = GetTempPathA(MAX_PATH, directory);
    if (length && length < MAX_PATH)
        write_to(directory, static_cast<int>(length), line, at);
}

void trace(const char* what) { trace(what, nullptr, 0, false); }
void trace(const char* what, DWORD value, bool has_value) {
    trace(what, nullptr, value, has_value);
}

const DWORD kProviderCount =
    static_cast<DWORD>(sizeof(snp_caps_records) / sizeof(snp_caps_records[0]));

BOOL CALLBACK CompareNetAddresses(SNETADDRPTR a, SNETADDRPTR b, DWORD* result) {
    if (result) *result = 0;
    if (!a || !b) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    /* Blizzard's own null provider writes !memcmp here: 1 means equal, despite
     * the parameter being named "diffmagnitude" (NULL.CPP:31). */
    if (result) *result = (memcmp(a, b, sizeof(SNETADDR)) == 0);
    return TRUE;
}

BOOL CALLBACK Destroy(void) {
    trace("Destroy");
    return TRUE;
}

BOOL CALLBACK Free(SNETADDRPTR addr, LPVOID data, DWORD) {
    if (!addr || !data) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;  /* we never hand storm a packet, so there is nothing to release */
}

BOOL CALLBACK FreeExternalMessage(SNETADDRPTR, LPVOID, DWORD) {
    SetLastError(SNP_ERROR_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL CALLBACK GetGameInfo(DWORD, LPCSTR, DWORD, SNP_GAMELIST_SC*) {
    SetLastError(SNP_ERROR_GAME_NOT_FOUND);
    return FALSE;
}

BOOL CALLBACK GetPerformanceData(DWORD, DWORD*, DWORD, DWORD) {
    SetLastError(SNP_ERROR_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL CALLBACK Initialize(SNETPROGRAMDATAPTR, SNETPLAYERDATAPTR, SNETUIDATAPTR,
                         SNETVERSIONDATAPTR, HANDLE) {
    /* Reaching here means a player selected this provider in the menu: the
     * archive, the enumeration and the caps filter have all been passed. */
    trace("Initialize -- storm bound to this provider");
    /* A real provider stores the HANDLE and SetEvent()s it whenever a packet
     * is queued; storm blocks on it. There is nothing to signal here. */
    return TRUE;
}

BOOL CALLBACK InitializeDevice(DWORD, LPVOID, LPVOID, DWORD*, LPVOID) {
    SetLastError(SNP_ERROR_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL CALLBACK LockDeviceList(DWORD* list) {
    if (list) *list = 0;  /* empty list: storm stops asking about devices */
    return TRUE;
}

BOOL CALLBACK LockGameList(DWORD, DWORD, SNP_GAMELIST_SC** list) {
    if (list) *list = nullptr;
    return TRUE;
}

BOOL CALLBACK Receive(SNETADDRPTR*, LPVOID*, DWORD*) {
    /* "Nothing waiting" is not a bare FALSE -- storm distinguishes it from a
     * real failure by the last-error value. */
    SetLastError(SNP_ERROR_NO_MESSAGES_WAITING);
    return FALSE;
}

BOOL CALLBACK ReceiveExternalMessage(SNETADDRPTR*, LPVOID*, DWORD*) {
    SetLastError(SNP_ERROR_NO_MESSAGES_WAITING);
    return FALSE;
}

BOOL CALLBACK SelectGame(DWORD, SNETPROGRAMDATAPTR, SNETPLAYERDATAPTR, SNETUIDATAPTR,
                         SNETVERSIONDATAPTR, DWORD*) {
    SetLastError(SNP_ERROR_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL CALLBACK Send(DWORD, SNETADDRPTR*, LPVOID, DWORD) {
    /* Storm passes up to 16 addresses in one call (SNET.CPP:856). A real
     * provider must walk the whole list, not just the first entry. */
    return TRUE;
}

BOOL CALLBACK SendExternalMessage(DWORD, DWORD, DWORD, DWORD, DWORD) {
    SetLastError(SNP_ERROR_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL CALLBACK StartAdvertisingGame(LPCSTR, LPCSTR, LPCSTR, DWORD, DWORD, DWORD, DWORD,
                                   DWORD, LPCVOID, DWORD) {
    return TRUE;
}

BOOL CALLBACK StopAdvertisingGame(void) { return TRUE; }

BOOL CALLBACK UnlockDeviceList(LPVOID) { return TRUE; }

BOOL CALLBACK UnlockGameList(SNP_GAMELIST_SC*, DWORD*) { return TRUE; }

/* Exactly SNP_MINSPISIZE bytes are claimed; everything past UnlockGameList
 * stays zero and storm never reads it. */
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
    /* slots 20-27 left NULL */
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
};

}  // namespace

extern "C" BOOL APIENTRY SnpQuery(DWORD index, DWORD* id, LPCSTR* description,
                                  LPCSTR* requirements, SNETCAPSPTR* caps) {
    if (!id || !description || !requirements || !caps) return FALSE;
    trace("SnpQuery index", index, true);
    /* The first FALSE ends storm's enumeration loop, and it is also what
     * bounds its walk through caps.dat -- the record count and this count are
     * the same number because they come from the same array. */
    if (index >= kProviderCount) return FALSE;

    const SNP_CAPS_RECORD& record = snp_caps_records[index];
    *id           = record.id;
    *description  = record.description;
    *requirements = record.requirements;
    /* Storm copies the caps out and never writes through the pointer
     * (SNET.CPP:601); in signed mode it prefers caps.dat's copy anyway. */
    *caps         = const_cast<SNETCAPSPTR>(&record.caps);
    return TRUE;
}

extern "C" BOOL APIENTRY SnpBind(DWORD index, void** spi) {
    if (!spi) return FALSE;
    trace("SnpBind index", index, true);
    if (index >= kProviderCount) return FALSE;

    *spi = &g_spi;
    return TRUE;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    /* Storm loads and frees the module once to enumerate it and again to bind.
     * Keep this free of anything expensive or stateful. */
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        char path[MAX_PATH] = "";
        GetModuleFileNameA(module, path, MAX_PATH);
        /* Reaching here means storm opened the appended archive, accepted it,
         * and called LoadLibrary. Naming the file says which module did, when
         * several are being compared. */
        trace("attach -- archive accepted, module loaded: ", path, 0, false);
    }
    return TRUE;
}
