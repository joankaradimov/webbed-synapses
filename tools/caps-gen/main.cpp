/*
 * Writes a module's caps.dat from the same declaration the module compiles.
 *
 *     snp-caps-<module> <output.dat>
 *
 * One executable per module: the build compiles this file with
 * SNP_CAPS_TABLE_HEADER pointing at that module's table, so there is exactly
 * one place where a provider's id, strings and capabilities are written down.
 *
 * The record format is storm's, not ours -- SNET.CPP:408-431, and confirmed
 * against both Blizzard's SOURCE/STANDARD/CAPS.DAT and the caps.dat inside
 * BWAPI's shipped 1.16.1 module. Per provider, in SnpQuery's index order:
 *
 *     DWORD bytes         length of everything after this field
 *     DWORD id            FOURCC
 *     char  description[] NUL-terminated
 *     char  requirements[] NUL-terminated
 *     SNETCAPS caps       caps.size bytes
 *
 * Storm rejects the file if `bytes` does not match what it measures, so the
 * length is computed here rather than declared anywhere.
 *
 * Little-endian is assumed, as it is everywhere else in this repository: the
 * only hosts that matter are x86 and x64.
 */

#include "snp/caps_record.h"

#ifndef SNP_CAPS_TABLE_HEADER
#error "SNP_CAPS_TABLE_HEADER must name the module's caps table header"
#endif
#include SNP_CAPS_TABLE_HEADER

#include <stdio.h>
#include <string.h>

static int write_all(FILE *out, const void *data, size_t bytes) {
    return fwrite(data, 1, bytes, out) == bytes;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <output.dat>\n", argc ? argv[0] : "snp-caps");
        return 2;
    }

    FILE *out = fopen(argv[1], "wb");
    if (!out) {
        fprintf(stderr, "snp-caps: cannot write %s\n", argv[1]);
        return 1;
    }

    const size_t count = sizeof(snp_caps_records) / sizeof(snp_caps_records[0]);
    for (size_t i = 0; i < count; ++i) {
        const SNP_CAPS_RECORD *record = &snp_caps_records[i];
        const size_t description = strlen(record->description) + 1;
        const size_t requirements = strlen(record->requirements) + 1;
        const DWORD bytes = (DWORD)(sizeof(DWORD) + description + requirements
                                    + record->caps.size);

        if (!write_all(out, &bytes, sizeof(bytes)) ||
            !write_all(out, &record->id, sizeof(record->id)) ||
            !write_all(out, record->description, description) ||
            !write_all(out, record->requirements, requirements) ||
            !write_all(out, &record->caps, record->caps.size)) {
            fprintf(stderr, "snp-caps: write failed\n");
            fclose(out);
            return 1;
        }
    }

    if (fclose(out) != 0) {
        fprintf(stderr, "snp-caps: write failed\n");
        return 1;
    }

    printf("snp-caps: wrote %s (%u provider%s)\n", argv[1], (unsigned)count,
           count == 1 ? "" : "s");
    return 0;
}
