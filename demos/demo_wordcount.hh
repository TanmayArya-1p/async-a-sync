#pragma once

#include "utils.hh"

static size_t demo_wordcount(void) {
    // invoke a read on all files and fill a buffer for each file
    read_all_files();

    size_t words = 0;
    for (int i = 0; i < demo_n; i++) {
        // count words in ith file's buffer
        words += wordcount(file_data(i));
    }

    return words;
}
