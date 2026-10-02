/* A pread into a buffer that a queued pwrite is still sending waits for the
 * pwrite: the pwrite's read mark orders it. Without that, both requests went
 * to the kernel in one batch, unordered, and the pwrite could send the
 * pread's bytes. Drives the io_uring runtime the way pass-emitted stubs do,
 * by hand, so the stock compiler builds it. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fasync.h"
#include "filc_async.h"
#include "filc_async_alloc.h"
#include "filc_async_runtime.h"

typedef struct {
    union {
        void* ptr;
        uint64_t word;
    } value;
    uint64_t capability;
} staged_arg;
_Static_assert(sizeof(staged_arg) == 16, "annotated argument slot must be 16 bytes");

static const char* const read_opts[] = { "op=pread", NULL };
static const char* const write_opts[] = { "op=pwrite", NULL };

static filc_async_meta* make_meta(const char* name, const char* const* opts,
                                  unsigned buffer_kind)
{
    filc_async_meta* m = filc_async_alloc(sizeof(*m) + 4 * sizeof(m->args[0]), 16);
    if (!m)
        return NULL;
    m->name = name;
    m->nargs = 4;
    m->noped_args = 1;
    m->result = FILC_ASYNC_RESULT_PTR;
    m->opts = opts;
    m->runtime = &filc_async_runtime_io_uring;
    m->args[1].kind = buffer_kind;
    return m;
}

static filc_async_meta* read_meta;
static filc_async_meta* write_meta;

/* What a stub does: start a task, mark an output pending or give an input a
 * read mark, submit it. */
static void* call(const filc_async_meta* m, int fd, void* buf, size_t len,
                  unsigned long offset)
{
    staged_arg* a = filc_async_alloc(4 * sizeof(staged_arg), 16);
    if (!a)
        exit(90);
    a[0].value.word = (unsigned)fd;
    a[1].value.ptr = buf;
    a[2].value.word = len;
    a[3].value.word = offset;
    void* task = filc_async_begin(m, a);
    if (m->args[1].kind == FILC_ASYNC_ARG_BUFFER_IN)
        filc_async_mark_input(task, buf);
    else
        filc_async_mark_pending(task, buf);
    filc_async_submit(task, m, NULL, a, 4);
    return task;
}

static long finish(void* pending)
{
    struct filc_async_result_s r = { 0 };
    r.pending = pending;
    filc_async_wait(&r);
    return r.state == 0 ? r.result : -1;
}

static unsigned long pending_resolves(void)
{
    filc_async_stats s;
    filc_async_get_stats(&s);
    return s.pending_resolves;
}

static int expect(const char* what, int ok)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    return ok;
}

static int open_file(const char* dir, const char* name, const char* content)
{
    char path[512];
    if (snprintf(path, sizeof path, "%s/t_war_%s_%ld.dat", dir, name,
                 (long)getpid()) >= (int)sizeof path)
        exit(91);
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0)
        exit(92);
    unlink(path);
    size_t len = strlen(content);
    if (len && write(fd, content, len) != (long)len)
        exit(93);
    return fd;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    read_meta = make_meta("read", read_opts, FILC_ASYNC_ARG_BUFFER_OUT);
    write_meta = make_meta("write", write_opts, FILC_ASYNC_ARG_BUFFER_IN);
    if (!read_meta || !write_meta)
        return 2;
    const filc_async_meta* table[] = { read_meta, write_meta, NULL };
    filc_async_validate_table(table);

    int out = open_file(dir, "out", "");
    int in = open_file(dir, "in", "second!!");
    char* buf = malloc(8);
    char* check = malloc(9);
    if (!buf || !check)
        return 3;
    memcpy(buf, "first!!!", 8);
    int ok = 1;

    /* pwrite buf, then pread into buf: the pread waits for the pwrite. */
    unsigned long before = pending_resolves();
    void* w = call(write_meta, out, buf, 8, 0);
    ok &= expect("an input is not pending", !filc_async_is_pending(buf));
    void* r = call(read_meta, in, buf, 8, 0);
    ok &= expect("the pread waited for the queued pwrite",
                 pending_resolves() == before + 1);
    ok &= expect("pwrite sent 8 bytes", finish(w) == 8);
    ok &= expect("pread read 8 bytes", finish(r) == 8);
    ok &= expect("the buffer holds what the pread read",
                 memcmp(buf, "second!!", 8) == 0);
    memset(check, 0, 9);
    ok &= expect("the file holds what the buffer held when written",
                 pread(out, check, 8, 0) == 8 && strcmp(check, "first!!!") == 0);

    /* Two pwrites from one buffer do not wait for each other. */
    before = pending_resolves();
    void* w1 = call(write_meta, out, buf, 8, 0);
    void* w2 = call(write_meta, out, buf, 8, 8);
    ok &= expect("two readers of one buffer do not wait",
                 pending_resolves() == before);
    ok &= expect("both pwrites complete", finish(w1) == 8 && finish(w2) == 8);
    memset(check, 0, 9);
    ok &= expect("the second pwrite landed",
                 pread(out, check, 8, 8) == 8 && strcmp(check, "second!!") == 0);

    close(out);
    close(in);
    printf("T_WRITE_AFTER_READ_URING %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
