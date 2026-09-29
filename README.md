# async-a-sync

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20x86--64-lightgrey.svg)](wiki/Getting-Started.md)
[![Language](https://img.shields.io/badge/language-C-informational.svg)](https://en.wikipedia.org/wiki/C_(programming_language))

async-a-sync lets selected C calls run asynchronously while keeping ordinary
call syntax. It is built on [Fil-C](https://github.com/pizlonator/fil-c), whose
capability and object machinery lets the compiler wait when a pending buffer is
accessed.

The project exists to keep asynchronous work separate from application code:
the compiler rewrites direct calls, a generic framework tracks tasks, buffers
and dependencies, and a runtime performs the operation. The repository ships
an io_uring runtime, but the framework can use other runtimes too.

## General principles

- An annotation names a runtime with `runtime=<name>`. A program can link more
  than one runtime; each call goes to the runtime named by its function.
- The generated stub takes dependency locks, marks output buffers pending, and
  hands the call to the runtime.
- `bin=` is read-only. `bout=`, `buf=`, and unannotated pointer arguments are
  treated as pending until completion.
- Read dependencies share a key. Write dependencies are exclusive and follow
  submission order.
- A runtime implements `submit`, `poll`, and `validate`. Completion clears
  pending marks, releases dependency locks, and wakes waiters.

## Small example

```c
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "filc_async.h"

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "fd=0", "bout=1"))), apply_to=function)
void* read_at(int fd, void* buf, size_t len, unsigned long offset)
{
    return 0;
}
#pragma clang attribute pop

int main(int argc, char** argv)
{
    int fd = open(argc > 1 ? argv[1] : "hello.txt", O_RDONLY);
    char buf[64] = {0};
    void* task = read_at(fd, buf, sizeof buf - 1, 0);

    putchar(buf[0]); // waits for the read

    struct filc_async_result_s result = { .pending = task };
    filc_async_wait(&result);
    close(fd);
    return result.state == 2;
}
```

The call returns a task handle. The first access to `buf` waits for the read;
there is no explicit submit or wait in the application code before that access.

## Build and run

Use Linux x86-64 with io_uring enabled. Fetch the Fil-C binary and the pinned
source revision as described in [Getting started](wiki/Getting-Started.md).
Then build the runtime and patched compiler:

```sh
./runtime/build.sh
JOBS=8 ./compiler/build.sh
```

Run the tests and an included demo:

```sh
./tests/run.sh
make demo-pragma-hello
```

Build your own program with the patched compiler. Link every runtime named by
your annotations before the framework and Fil-C libraries:

```sh
vendor/fil-c-src/build/bin/filcc -O2 -static \
  -Werror=pragma-clang-attribute \
  -Iruntime/src -Lruntime/build/lib \
  -o app app.c -lfilc_async_uring -lpizlo -lc
./app hello.txt
```

The `runtime=io_uring` annotation requires `-lfilc_async_uring`. A different
runtime supplies its own descriptor and library. See [Build and link](wiki/Building-and-Linking.md)
for the full link rules.

## Documentation

- [Getting started](wiki/Getting-Started.md)
- [Annotation reference](wiki/Annotation-Reference.md)
- [Runtime API](wiki/Runtime-API.md)
- [Framework API](wiki/Framework-API.md)
- [Write a runtime](wiki/Writing-a-Runtime.md)
- [Limitations](wiki/Limitations.md)
- [Tests](wiki/Testing.md)

io_uring may be blocked by a container seccomp profile. The current runtime
also has documented limitations around multi-key dependency cycles, concurrent
result collection, explicit token concurrency, effect-set ranges, and pending
file-descriptor capacity.
