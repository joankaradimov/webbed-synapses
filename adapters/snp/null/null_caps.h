/*
 * The null provider's declaration. Compiled twice: into the module, where
 * SnpQuery answers out of it, and into the host tool that writes caps.dat.
 *
 * The capabilities are BWAPI DirectIP's, copied field for field [W] -- the
 * caps.dat inside its shipped caps.mpq, which is the only set of numbers known
 * to produce a menu entry in StarCraft 1.16.1. That is the whole reason for
 * copying them rather than describing this provider honestly.
 *
 * Being enumerated is not the same as being listed. Storm fills the provider
 * list box only with entries that pass SpiMeetsMinimumCaps against the caps the
 * *game* asks for (SNET.CPP:2853-2861):
 *
 *     (mincaps->flags & curr->flags) == mincaps->flags
 *     mincaps->maxmessagesize <= curr->maxmessagesize
 *     mincaps->maxqueuesize   <= curr->maxqueuesize
 *     mincaps->maxplayers     <= curr->maxplayers
 *     mincaps->bytessec       <= curr->bytessec
 *     !mincaps->latencyms || mincaps->latencyms >= curr->latencyms
 *
 * so every field except latency wants to be generous, and a provider that
 * describes itself modestly disappears with no error anywhere. Blizzard's own
 * null provider declares maxplayers 1 -- and an empty description, because it
 * was never meant to appear.
 */
#ifndef SNP_NULL_CAPS_H
#define SNP_NULL_CAPS_H

#include "snp/caps_record.h"

static const SNP_CAPS_RECORD snp_caps_records[] = {
    {
        'SNUL',
        "Null (no network)",
        "None. This provider carries no traffic.",
        {
            sizeof(SNETCAPS),
            /* PAGELOCKEDBUFFERS | BASICINTERFACE, as DirectIP and Blizzard's
             * LAN providers declare. 1.16.1 does not require either: its own
             * battle.snp declares flags 0 and is always listed, so the game's
             * mincaps.flags is 0 (docs/snp-abi.md section 2). The first makes
             * storm VirtualLock an 8 KB send buffer, which costs nothing. */
            SNET_CAPS_PAGELOCKEDBUFFERS | SNET_CAPS_BASICINTERFACE,
            500,     /* maxmessagesize        -- storm subtracts 16 */
            16,      /* maxqueuesize          -- 16 in every Blizzard provider */
            256,     /* maxplayers            -- generous on purpose, see above */
            1000,    /* bytessec */
            50,      /* latencyms             -- drives storm's timeouts */
            8,       /* defaultturnssec */
            2        /* defaultturnsintransit */
        }
    }
};

#endif /* SNP_NULL_CAPS_H */
