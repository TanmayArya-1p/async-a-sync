#pragma once

#include <string.h>

#include "fasync_dep.h"
#include "../common/utils.hh"

static int demo_provenance(int fdw, int fdr) {
    int verified = 0;

    // No provenance token: no dependency between the two calls
    fasync_id w = fasync_pwrite(fdw, demo_prov_author, demo_prov_len, 0);
    fasync_id r = fasync_pread(fdr, demo_prov_plain_reader, demo_prov_len, 0);

    if (w && r && !fasync_ready(w) && !fasync_ready(r) &&
        fasync_result(w) == (long)demo_prov_len &&
        fasync_result(r) == (long)demo_prov_len) {
        verified++;
    }

    if (demo_prov_resterile(fdw) < 0) {
        return verified;
    }

    // Provenance token: the read is ordered after the write
    fasync_tracker *tag = fasync_tracker_new();
    if (!tag) {
        return verified;
    }
    w = fasync_tagged_pwrite(fdw, demo_prov_author, demo_prov_len, 0, tag, FASYNC_INOUT);
    r = fasync_tagged_pread(fdr, demo_prov_tagged_reader, demo_prov_len, 0, tag, FASYNC_INOUT);

    if (w && r && fasync_ready(w) && fasync_result(w) == (long)demo_prov_len && fasync_result(r) == (long)demo_prov_len &&
    memcmp(demo_prov_author, demo_prov_tagged_reader, demo_prov_len) == 0) {
        verified++;
    }

    fasync_tracker_free(tag);
    return verified;
}
