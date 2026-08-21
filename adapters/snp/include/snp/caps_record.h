/*
 * A provider's declaration: what SnpQuery answers, and what caps.dat says.
 *
 * Storm never sees this struct -- it is ours, so nothing here is layout
 * critical and tests/abi_layout.cpp has nothing to say about it. What it
 * prevents is drift. In signed mode storm takes the id, description and
 * requirement string from caps.dat and uses SnpQuery's only as a liveness
 * check (SNET.CPP:553-590), and it walks caps.dat one record per index that
 * SnpQuery answers, so the two files have to agree on count, order and
 * content. Declaring them once, in a header that both the module and the
 * caps.dat generator compile, is what makes that automatic.
 *
 * See docs/snp-abi.md section 2 for the on-disk record format.
 */
#ifndef SNP_CAPS_RECORD_H
#define SNP_CAPS_RECORD_H

#include "snp/storm_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SNP_CAPS_RECORD {
    DWORD       id;             /* FOURCC, e.g. 'SNUL' */
    const char *description;    /* what the network-selection menu shows */
    const char *requirements;   /* the "requires..." line under it */
    SNETCAPS    caps;
} SNP_CAPS_RECORD;

/* A module defines snp_caps_records[] in a header of its own and names it to
 * add_snp_module(... CAPS <header>); tools/caps-gen compiles that header to
 * write the archive's caps.dat. */

#ifdef __cplusplus
}
#endif

#endif /* SNP_CAPS_RECORD_H */
