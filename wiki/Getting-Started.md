# Getting started

This tutorial covers:

- building the patched Fil-C compiler and the async libraries;
- running the test suite and a demo;
- writing a program with one annotated call and watching it run
  asynchronously.

It takes about an hour, most of it the Clang build.

## Before you begin

You need a Linux x86-64 machine where io_uring is allowed. You also need:

- CMake, Ninja, Git, Ruby, a host C/C++ compiler (Clang for the runtime
  build), and Linux headers;
- about 8 GiB of free RAM for the Clang build at `JOBS=8`, and a few GiB of disk.

io_uring is often blocked in the following environments:

- **Docker:** run the container with `--security-opt seccomp=unconfined`.
- **Rosetta:** an x86-64 container on Apple silicon runs under Rosetta, which
  does not implement io_uring at all.

## 1. Fetch Fil-C

The repository needs two Fil-C trees under `vendor/`:

- **The 0.685 binary distribution.** It provides `pizfix` and the stock `filcc`.
- **A source checkout.** Its Clang sources are what this repository patches.

```sh
mkdir -p vendor
curl -L -o vendor/filc-0.685-linux-x86_64.tar.xz \
  https://github.com/pizlonator/fil-c/releases/download/v0.685/filc-0.685-linux-x86_64.tar.xz
tar -C vendor -xf vendor/filc-0.685-linux-x86_64.tar.xz

git clone --depth 1 --filter=blob:none --sparse -b deluge \
  https://github.com/pizlonator/fil-c.git vendor/fil-c-src
git -C vendor/fil-c-src sparse-checkout set \
  clang cmake filc libpas lld llvm third-party
# the revision the compiler patches are tested on
git -C vendor/fil-c-src fetch --depth 1 --filter=blob:none origin \
  d80c8bba1c58f68c33b0ed5e71113c44354f5bb8
git -C vendor/fil-c-src checkout FETCH_HEAD
```

## 2. Build

```sh
./runtime/build.sh           # the framework and the runtimes
JOBS=8 ./compiler/build.sh   # the patched clang: the long step
```

When both finish, you have:

- `runtime/build/lib/libpizlo.a`, which holds Fil-C's runtime, the async
  framework and the native bridges;
- `runtime/build/lib/libfilc_async_uring.a`, the io_uring runtime;
- `vendor/fil-c-src/build/bin/filcc`, the patched compiler.

## 3. Run the tests and a demo

```sh
./tests/run.sh
make demo-pragma-hello
```

`tests/run.sh` ends with a summary of passed, failed and skipped tests. With
io_uring available and both builds present, nothing should fail or be
skipped. The hello demo makes one annotated call, shows where the program
waits for it, and ends with `DEMO OK`.

## 4. Write an annotated call

Save this as `first.c`:

```c
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "filc_async.h"

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf"))), apply_to=function)
void* read_at(int fd, void* buf, size_t len, unsigned long offset);
#pragma clang attribute pop

// The io_uring runtime runs this body before it issues the read.
void* read_at(int fd, void* buf, size_t len, unsigned long offset)
{
    printf("reading %zu bytes from fd %d\n", len, fd);
    return 0;
}

int main(int argc, char** argv)
{
    int fd = open(argc > 1 ? argv[1] : "hello.txt", O_RDONLY);
    if (fd < 0)
        return 1;

    static char buf[64];
    void* task = read_at(fd, buf, sizeof buf - 1, 0); // returns at once
    printf("pending after the call: %d\n", filc_async_is_pending(buf));

    printf("first byte: %c\n", buf[0]);               // waits for the read here

    struct filc_async_result_s r = { .pending = task };
    filc_async_wait(&r);
    printf("read returned %ld\n", r.result);
    close(fd);
    return 0;
}
```

The pragma tells the compiler three things:

- calls to `read_at` run on the io_uring runtime (`runtime=io_uring`), which
  the link line brings in with `-lfilc_async_uring`;
- `read_at` stands for `pread` (`op=pread`);
- `buf` is a buffer the call fills (`bout=buf`). Options name parameters,
  not positions.

Build and run it:

```sh
echo hello > hello.txt
vendor/fil-c-src/build/bin/filcc -O2 -static -Werror=pragma-clang-attribute \
  -Iruntime/include -Lruntime/build/lib -o first first.c \
  -lfilc_async_uring -lpizlo -lc
./first hello.txt
```

```text
reading 63 bytes from fd 3
pending after the call: 1
first byte: h
read returned 6
```

## What happened

1. **The call.** The compiler redirected the call to `read_at` into a stub. The
   stub marked `buf` pending and handed the call to the io_uring runtime. The
   runtime ran the body, which printed the first line, and queued a read
   request. The call then returned a task handle without entering the
   kernel.
2. **`filc_async_is_pending(buf)`.** It reported the buffer as still owned by
   the call.
3. **`buf[0]`.** Before every load through a pointer, the compiler inserts a
   test of the pending flag in the object's header. The flag was set, so the
   load waited. That wait sent the queued request to the kernel and collected
   its completion.
4. **`filc_async_wait`.** It delivered the call's result: the byte count, or
   `-errno` on failure.

No submit or wait call was needed before reading the buffer. `filc_async_wait`
is only there to get the result.

## Next steps

- [Annotate a function](Annotating-Functions.md) covers input buffers,
  dependencies between calls, and calls declared in headers.
- [Architecture](Architecture.md) explains what the compiler and the framework
  did in each step above.
- [Write a runtime](Writing-a-Runtime.md) shows how to put something other than
  io_uring behind the same calls.
