#pragma once

#include "utils.hh"

static int demo_plain_io(void) {
    for (int i = 0; i < demo_n; i++) {
        if (!fasync_pread(demo_fd[i], demo_buf[i], demo_bytes, 0)) {
            return 0;
        }
    }

    int correct = 0;
    for (int i = 0; i < demo_n; i++) {
        if (wordcount((const char*)demo_buf[i]) == demo_expect[i]) {
            correct++;
        }
    }

    return correct;
}
