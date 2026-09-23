#pragma once
#include <stddef.h>

/* Generic allocator interface for the filc_async machinery. Every byte the
 * async runtime allocates flows through here -- never malloc at call sites,
 * and never in backend code. The default allocator is the bump arena in
 * filc_async_arena.c (zgc-backed, zeroed memory).
 *
 * Contract: alloc(size, align) returns `size` bytes, zero initialized, aligned
 * to a power-of-two `align`, or NULL on failure. free(p, size) releases an
 * allocation obtained from the same allocator; the arena's free is a no-op
 * (arena memory lives for the process).
 */

typedef struct {
    void* (*alloc)(size_t size, size_t align); /* returns zeroed memory */
    void  (*free) (void* p, size_t size);
} filc_async_allocator;

/* Install allocator `a`. Must be called from the user program before any use
 * of the async machinery. Ruling-6: an ALL-ZEROS allocator {0,0} is not a
 * real allocator -- it means "restore the default arena allocator". */
void filc_async_set_allocator(filc_async_allocator a);
filc_async_allocator filc_async_get_allocator(void);

/* The symbol Fil-C generates at rewritten call sites: route the request
 * through whatever allocator is currently installed (honoring swaps). */
void* filc_async_alloc(size_t size, size_t align);