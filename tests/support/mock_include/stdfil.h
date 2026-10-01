#pragma once
#include <stddef.h>

void* zgetlower(void* ptr);
void* zgetupper(void* ptr);
void* zgc_aligned_alloc(size_t alignment, size_t size);
