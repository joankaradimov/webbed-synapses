/*
 * The WebRTC provider's declaration. Compiled into the module, where SnpQuery
 * answers out of it, and into the host tool that writes caps.dat -- storm
 * shows the strings from caps.dat (docs/snp-abi.md section 2), so the two
 * must never disagree.
 *
 * The capabilities are BWAPI DirectIP's [W], the only set proven to reach
 * StarCraft 1.16.1's menu. They also describe this transport honestly enough:
 * datagrams of up to 500 bytes, no hard player limit, and a latency figure
 * storm uses to size its timeouts (docs/snp-abi.md section 3). The 50 ms
 * figure is optimistic for a relayed Internet path; revisit it once real games
 * have been played over one.
 */
#ifndef SNP_WEBRTC_CAPS_H
#define SNP_WEBRTC_CAPS_H

#include "snp/caps_record.h"

static const SNP_CAPS_RECORD snp_caps_records[] = {
    {
        'WRTC',
        "WebRTC (Internet)",
        "An Internet connection. The signalling server is set in webbed.ini "
        "beside this module.",
        {
            sizeof(SNETCAPS),
            SNET_CAPS_PAGELOCKEDBUFFERS | SNET_CAPS_BASICINTERFACE,
            500,     /* maxmessagesize        -- storm subtracts 16 */
            16,      /* maxqueuesize */
            256,     /* maxplayers */
            1000,    /* bytessec */
            50,      /* latencyms */
            8,       /* defaultturnssec */
            2        /* defaultturnsintransit */
        }
    }
};

#endif /* SNP_WEBRTC_CAPS_H */
