#pragma once

#include <stddef.h>

long zsys_write(int fd, const void* buf, size_t len);
void zsys_abort(void);