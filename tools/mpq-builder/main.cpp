/*
 * A directory tree in, one MPQ archive out.
 *
 *     snp-mpq <input-directory> <output.mpq>
 *
 * This exists because storm will not enumerate a .snp that is not also a
 * readable MPQ containing caps.dat (docs/snp-abi.md section 2), and the
 * archive is small enough that generating it beats shipping someone else's.
 *
 * Three deliberate choices:
 *
 *  - Files are stored, not compressed. The payload here is a few hundred bytes,
 *    and it removes any question about which compressions the storm.dll of a
 *    given game accepts on this path.
 *  - The archive is forced to MPQ v1, which is all a 1998-era reader knows.
 *  - No staleness check of its own. It is driven by a custom *command* with
 *    declared outputs, so CMake decides when to re-run it.
 *
 * Signing is opt-in here (--sign) and on by default in the build. Storm skips
 * a module whose archive has no signature, or one that does not verify
 * (SNET.CPP:731-737; 1.16.1's storm.dll does the same, docs/snp-abi.md
 * section 2), so the game needs it. StarCraft lists a module carrying a
 * weak-signed archive.
 */

#include <StormLib.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// MPQ path separators are backslashes regardless of host, and lookups are
// case-insensitive. Storm asks for "caps.dat".
std::string archive_name(const fs::path &root, const fs::path &file) {
    std::string name = fs::relative(file, root).generic_string();
    std::replace(name.begin(), name.end(), '/', '\\');
    return name;
}

int fail(const char *what) {
    std::fprintf(stderr, "snp-mpq: %s failed (error %u)\n", what,
                 static_cast<unsigned>(GetLastError()));
    return 1;
}

// Reading is not this tool's job, but it is the only reader here that copes
// with PKWARE-imploded, encrypted files -- which is what Blizzard's own
// CAPS.MPQ and BWAPI's both turn out to contain. tools/inspect-snp.mjs reads
// the archives we produce without StormLib, on purpose; this is for theirs.
int extract(const char *archive, const char *name, const char *output) {
    HANDLE mpq = NULL;
    if (!SFileOpenArchive(archive, 0, STREAM_FLAG_READ_ONLY, &mpq))
        return fail("SFileOpenArchive");
    if (!SFileHasFile(mpq, name)) {
        std::fprintf(stderr, "snp-mpq: %s holds no %s\n", archive, name);
        SFileCloseArchive(mpq);
        return 1;
    }
    if (!SFileExtractFile(mpq, name, output, SFILE_OPEN_FROM_MPQ)) {
        SFileCloseArchive(mpq);
        return fail("SFileExtractFile");
    }
    SFileCloseArchive(mpq);
    std::printf("snp-mpq: extracted %s from %s to %s\n", name, archive, output);
    return 0;
}

// Storm rejects an archive whose signature is present and does not verify, so
// a signature that does not survive being appended to a module is worse than
// none at all. This answers that by opening the finished .snp.
int verify(const char *archive) {
    HANDLE mpq = NULL;
    if (!SFileOpenArchive(archive, 0, STREAM_FLAG_READ_ONLY, &mpq))
        return fail("SFileOpenArchive");

    const DWORD result = SFileVerifyArchive(mpq);
    SFileCloseArchive(mpq);

    const char *meaning = "unknown";
    switch (result) {
        case ERROR_NO_SIGNATURE:
            meaning = "no signature (storm accepts this: "
                      "SFILE_AUTH_UNABLETOAUTHENTICATE)";
            break;
        case ERROR_VERIFY_FAILED:
            meaning = "signature present but VERIFY FAILED -- storm rejects this";
            break;
        case ERROR_WEAK_SIGNATURE_OK:
            meaning = "weak signature, verified";
            break;
        case ERROR_WEAK_SIGNATURE_ERROR:
            meaning = "weak signature, INVALID -- storm rejects this";
            break;
        case ERROR_STRONG_SIGNATURE_OK:
            meaning = "strong signature, verified";
            break;
        case ERROR_STRONG_SIGNATURE_ERROR:
            meaning = "strong signature, INVALID -- storm rejects this";
            break;
    }
    std::printf("snp-mpq: %s: %s\n", archive, meaning);
    return (result == ERROR_VERIFY_FAILED ||
            result == ERROR_WEAK_SIGNATURE_ERROR ||
            result == ERROR_STRONG_SIGNATURE_ERROR) ? 1 : 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 5 && std::string(argv[1]) == "--extract")
        return extract(argv[2], argv[3], argv[4]);
    if (argc == 3 && std::string(argv[1]) == "--verify")
        return verify(argv[2]);

    bool sign = false;
    if (argc == 4 && std::string(argv[1]) == "--sign") {
        sign = true;
        ++argv;
        --argc;
    }

    if (argc != 3) {
        std::fprintf(stderr,
                     "Usage: %s [--sign] <input-directory> <output.mpq>\n"
                     "       %s --extract <archive> <name> <output>\n"
                     "       %s --verify <archive or module.snp>\n",
                     argv[0] ? argv[0] : "snp-mpq",
                     argv[0] ? argv[0] : "snp-mpq",
                     argv[0] ? argv[0] : "snp-mpq");
        return 2;
    }

    const fs::path root = argv[1];
    const fs::path output = argv[2];

    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        std::fprintf(stderr, "snp-mpq: not a directory: %s\n",
                     root.string().c_str());
        return 2;
    }

    std::vector<fs::path> files;
    for (const auto &entry : fs::recursive_directory_iterator(root, ec)) {
        if (entry.is_regular_file())
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());   // deterministic output

    if (files.empty()) {
        std::fprintf(stderr, "snp-mpq: %s holds no files; an empty archive is "
                             "never what was meant\n", root.string().c_str());
        return 2;
    }

    fs::create_directories(output.parent_path(), ec);
    fs::remove(output, ec);

    // The hash table is rounded up to a power of two with a floor of four, so
    // this is a hint rather than a limit; leaving room for the (listfile)
    // keeps the table from being exactly full.
    const DWORD capacity = static_cast<DWORD>(files.size()) * 2 + 2;

    HANDLE mpq = NULL;
    if (!SFileCreateArchive(output.string().c_str(),
                            MPQ_CREATE_ARCHIVE_V1 | MPQ_CREATE_LISTFILE,
                            capacity, &mpq))
        return fail("SFileCreateArchive");

    for (const auto &file : files) {
        const std::string name = archive_name(root, file);
        if (!SFileAddFile(mpq, file.string().c_str(), name.c_str(),
                          MPQ_FILE_REPLACEEXISTING)) {
            SFileCloseArchive(mpq);
            return fail("SFileAddFile");
        }
        std::printf("  + %s (%ju bytes, stored)\n", name.c_str(),
                    static_cast<std::uintmax_t>(fs::file_size(file, ec)));
    }

    // Blizzard's weak signature. It covers only the archive itself
    // (SFileVerify.cpp:163, BeginMpqData = ha->MpqPos), so it stays valid once
    // the archive is appended to a module -- `--verify` on a finished .snp
    // confirms it.
    if (sign) {
        if (!SFileSignArchive(mpq, SIGNATURE_TYPE_WEAK)) {
            SFileCloseArchive(mpq);
            return fail("SFileSignArchive");
        }
        std::printf("  + (signature), weak\n");
    }

    if (!SFileCloseArchive(mpq))
        return fail("SFileCloseArchive");

    std::printf("snp-mpq: wrote %s (%ju bytes)\n", output.string().c_str(),
                static_cast<std::uintmax_t>(fs::file_size(output, ec)));
    return 0;
}
