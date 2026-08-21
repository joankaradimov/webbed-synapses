/*
 * Loads a .snp the way storm does, and reports what a module answers.
 *
 *     snp-load <module.snp>
 *
 * Storm's enumeration is four steps (SNET.CPP:526-590): read caps.dat out of
 * the appended archive, LoadLibrary, GetProcAddress("SnpQuery"), then call it
 * with index 0, 1, 2... until it returns FALSE. Binding is LoadLibrary again
 * plus GetProcAddress("SnpBind") (SNET.CPP:672-712). This does all of that
 * except the archive, which tools/inspect-snp.mjs checks separately -- and it
 * does the load twice, because storm does: once to enumerate, once to bind.
 *
 * It cannot tell you whether the game will *list* the provider. That depends
 * on the caps the game asks for, which only the game knows
 * (SpiMeetsMinimumCaps, SNET.CPP:2853-2861). What it does tell you is whether
 * the module loads at all, exports what storm looks up, and describes itself
 * consistently -- which is most of what goes wrong.
 *
 * Must be built 32-bit, like everything else that touches a module.
 */

#include <windows.h>
#include <stdio.h>

#include "snp/storm_spi.h"

namespace {

typedef BOOL(APIENTRY *SnpQueryFn)(DWORD, DWORD *, LPCSTR *, LPCSTR *, SNETCAPSPTR *);
typedef BOOL(APIENTRY *SnpBindFn)(DWORD, void **);

int problems = 0;

void check(bool ok, const char *what) {
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++problems;
}

const char *fourcc(DWORD id, char *out) {
    out[0] = (char)((id >> 24) & 0xFF);
    out[1] = (char)((id >> 16) & 0xFF);
    out[2] = (char)((id >> 8) & 0xFF);
    out[3] = (char)(id & 0xFF);
    out[4] = '\0';
    return out;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <module.snp>\n", argc ? argv[0] : "snp-load");
        return 2;
    }

    printf("%s\n", argv[1]);

    // Enumeration pass.
    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) {
        printf("  FAIL  LoadLibrary failed with error %lu -- storm would skip "
               "this module in silence\n", GetLastError());
        return 1;
    }
    check(true, "LoadLibrary");

    SnpQueryFn query = (SnpQueryFn)GetProcAddress(module, "SnpQuery");
    SnpBindFn bind = (SnpBindFn)GetProcAddress(module, "SnpBind");
    check(query != NULL, "GetProcAddress(\"SnpQuery\")");
    check(bind != NULL, "GetProcAddress(\"SnpBind\")");
    if (!query || !bind) {
        FreeLibrary(module);
        return 1;
    }

    DWORD count = 0;
    for (DWORD index = 0;; ++index) {
        DWORD id = 0;
        LPCSTR description = NULL;
        LPCSTR requirements = NULL;
        SNETCAPSPTR caps = NULL;
        if (!query(index, &id, &description, &requirements, &caps))
            break;

        char name[5];
        printf("\n  provider %lu: '%s'\n", index, fourcc(id, name));
        printf("    description  \"%s\"\n", description ? description : "(null)");
        printf("    requirements \"%s\"\n", requirements ? requirements : "(null)");

        // Storm's own two rejection rules, SNET.CPP:566-575.
        check(caps && caps->size >= sizeof(SNETCAPS),
              "caps.size >= sizeof(SNETCAPS)");
        if (caps) {
            check(!(caps->flags & SNET_CAPS_DEBUGONLY),
                  "not flagged DEBUGONLY (a retail game drops those)");
            printf("    flags 0x%lx, maxmessagesize %lu (storm shows %lu), "
                   "maxqueuesize %lu,\n    maxplayers %lu, bytessec %lu, "
                   "latencyms %lu, %lu turns/sec, %lu in transit\n",
                   caps->flags, caps->maxmessagesize, caps->maxmessagesize - 16,
                   caps->maxqueuesize, caps->maxplayers, caps->bytessec,
                   caps->latencyms, caps->defaultturnssec,
                   caps->defaultturnsintransit);
            check(caps->maxplayers > 1,
                  "maxplayers > 1 (a game asking for two is a common minimum)");
        }
        check(description && *description,
              "the description is not empty (it is the menu entry)");
        ++count;
        if (index > 32) {
            check(false, "SnpQuery never returned FALSE");
            break;
        }
    }
    printf("\n");
    check(count > 0, "SnpQuery answered at least once");

    FreeLibrary(module);

    // Bind pass. Storm loads the module a second time for this.
    module = LoadLibraryA(argv[1]);
    if (!module) {
        check(false, "the second LoadLibrary, which storm does to bind");
        return 1;
    }
    bind = (SnpBindFn)GetProcAddress(module, "SnpBind");
    void *spi = NULL;
    const bool bound = bind && bind(0, &spi) && spi;
    check(bound, "SnpBind(0) returns a table");
    if (bound) {
        const DWORD size = *(DWORD *)spi;
        printf("    table size %lu bytes\n", size);
        check(size >= SNP_MINSPISIZE,
              "the table is at least MINSPISIZE (80 bytes: size + 19 slots)");

        // Every slot storm may call has to be non-NULL up to the claimed size.
        const DWORD slots = (size / sizeof(void *)) - 1;
        void **entries = ((void **)spi) + 1;
        bool filled = true;
        for (DWORD i = 0; i < slots; ++i)
            if (!entries[i])
                filled = false;
        check(filled, "no NULL slot below the claimed size");
    }
    FreeLibrary(module);

    printf("\n%s\n", problems ? "the module would not work" :
                                "the module behaves the way storm expects");
    return problems ? 1 : 0;
}
