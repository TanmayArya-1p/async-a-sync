#pragma once

// What the explicit-API demos share. Each demo's own setup and report are
// in <demo>_utils.hh, so each demo file holds only its fasync_* calls.

#include "fasync.h"
#include "fasync_dep.h"
#include "../common/utils.hh"

static inline const char* dir_arg(int argc, char** argv) {
  return argc > 1 ? argv[1] : "/tmp";
}

// Kernel entries and requests so far.
static inline struct fasync_stats io_stats(void) {
  struct fasync_stats s;
  fasync_get_stats(&s);
  return s;
}
