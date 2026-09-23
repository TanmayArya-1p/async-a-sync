#include <stdfil.h>
#include <stddef.h>
#include <stdint.h>

#include "filc_async_alloc.h"

/* Default bump arena for the filc_async allocator interface.
 *
 * Blocks come from zgc_aligned_alloc, exactly like the io_uring ring memory
 * in fasync.c; zgc memory is zero-initialized, so fresh chunks need no memset.
 *
 * A request the current bump block cannot satisfy -- size larger than the
 * normal payload, or alignment stricter than a block's 4096 guarantee -- gets
 * its OWN dedicated chained block. The bump pointer never wraps past its
 * block end: on exhaustion we chain a new block. free() is a no-op: arena
 * blocks live for the process. An all-zeros allocator {0,0} restores this
 * default (see filc_async_set_allocator).
 */

#define ARENA_NORMAL_BLOCK 65536u
#define ARENA_MAX_ALIGN    4096u

struct arena_block {
    struct arena_block* next; /* older block */
};

// Every block stays reachable from here, rooting its payloads for the GC.
static struct arena_block* g_blocks;

// Bump state for the current normal-size block.
static unsigned char* g_cur;
static unsigned char* g_end;

// Allocate a normal bump block, chain it, make it current.
static int arena_chain_normal(void)
{
    struct arena_block* b = (struct arena_block*)zgc_aligned_alloc(
        ARENA_MAX_ALIGN, sizeof(struct arena_block) + ARENA_NORMAL_BLOCK);
    if (!b)
        return 0;
    b->next = g_blocks;
    g_blocks = b;
    g_cur = (unsigned char*)(b + 1);
    g_end = g_cur + ARENA_NORMAL_BLOCK;
    return 1;
}

static void arena_chain(struct arena_block* b)
{
    b->next = g_blocks;
    g_blocks = b;
}

// Dedicated block for an oversized or over-aligned request; returns the
// aligned payload.
static void* arena_dedicated(size_t size, size_t align)
{
    // `align` extra bytes cover worst-case padding between the header and
    // the aligned payload start.
    struct arena_block* b = (struct arena_block*)zgc_aligned_alloc(
        align, sizeof(struct arena_block) + size + align);
    if (!b)
        return NULL;
    unsigned char* payload = (unsigned char*)(
        ((uintptr_t)((unsigned char*)(b + 1)) + align - 1) &
        ~((uintptr_t)align - 1));
    arena_chain(b);
    return payload;
}

static void* arena_alloc(size_t size, size_t align)
{
    if (!size)
        size = 1;
    if (align < 1)
        align = 1;
    if (align & (align - 1))
        return NULL; /* alignment must be a power of two */

    // Anything the normal bump block cannot hold gets a dedicated block.
    if (align > ARENA_MAX_ALIGN || size + align > ARENA_NORMAL_BLOCK)
        return arena_dedicated(size, align);

    uintptr_t base = (uintptr_t)g_cur;
    uintptr_t aligned = (base + align - 1) & ~((uintptr_t)align - 1);
    if (!g_cur || aligned + size > (uintptr_t)g_end) {
        // Exhausted: chain a new block and continue bumping there.
        if (!arena_chain_normal())
            return NULL;
        base = (uintptr_t)g_cur;
        aligned = (base + align - 1) & ~((uintptr_t)align - 1);
    }
    g_cur = (unsigned char*)(aligned + size);
    return (void*)aligned;
}

static void arena_free(void* p, size_t size)
{
    // Arena: nothing is ever freed.
    (void)p;
    (void)size;
}

static filc_async_allocator g_allocator = { arena_alloc, arena_free };

void filc_async_set_allocator(filc_async_allocator a)
{
    if (!a.alloc && !a.free) {
        // All-zeros allocator restores the default arena.
        g_allocator = (filc_async_allocator){ arena_alloc, arena_free };
        return;
    }
    g_allocator = a;
}

filc_async_allocator filc_async_get_allocator(void)
{
    return g_allocator;
}

void* filc_async_alloc(size_t size, size_t align)
{
    return filc_async_get_allocator().alloc(size, align);
}