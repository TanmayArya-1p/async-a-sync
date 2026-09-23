#pragma once
#include <stddef.h>

/* Allocator interface for the filc_async machinery. Every byte the runtime
 * allocates goes through here: never malloc at call sites, never in backend
 * code. Default allocator is the bump arena in filc_async_arena.c
 * (zgc-backed, zeroed memory).
 *
 * Contract: alloc(size, align) returns `size` zero-initialized bytes aligned
 * to a power-of-two `align`, or NULL on failure. free(p, size) releases an
 * allocation from the same allocator; the arena's free is a no-op.
 */

typedef struct {
    void* (*alloc)(size_t size, size_t align); /* returns zeroed memory */
    void  (*free) (void* p, size_t size);
} filc_async_allocator;

/* Install allocator `a`, from the user program before any async use. An
 * ALL-ZEROS allocator {0,0} means "restore the default arena". */
void filc_async_set_allocator(filc_async_allocator a);
filc_async_allocator filc_async_get_allocator(void);

/* The symbol Fil-C generates at rewritten call sites: route the request
 * through whatever allocator is currently installed. */
void* filc_async_alloc(size_t size, size_t align);