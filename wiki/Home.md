# async-a-sync wiki

async-a-sync is a compiler pass and runtime framework built on
[Fil-C](https://github.com/pizlonator/fil-c). They let ordinary, blocking-looking C
calls run asynchronously.

- You annotate a function.
- Every call to it returns at once.
- The first access to a buffer the call produces waits for the call to finish.

The framework does not depend on any runtime. Each annotated function names
the runtime that runs it, and one program can use several. This repository
ships two: io_uring, which issues calls as io_uring requests, and rpc, which
sends them to the demos' TCP server.

The pages below follow the [Diátaxis](https://diataxis.fr) layout. Tutorials
teach, how-to guides solve one task, reference pages describe the interfaces
exactly, and explanation pages cover the design.

## Tutorials

- [Getting started](Getting-Started.md): build the toolchain, run the tests and
  demos, and write your first annotated call.

## How-to guides

- [Annotate a function](Annotating-Functions.md): turn a function into an
  asynchronous call, declare its buffers and its dependencies.
- [Write a runtime](Writing-a-Runtime.md): implement a runtime's three
  functions and link it next to or instead of io_uring.
- [Build and link](Building-and-Linking.md): build the patched compiler and the
  libraries, and link a program against them.
- [Troubleshooting](Troubleshooting.md): common build, link and runtime failures.

## Reference

- [Annotation reference](Annotation-Reference.md): every `filc_async` option and
  its encoding in the descriptor.
- [Runtime API](Runtime-API.md): `filc_async_runtime.h`, the contract between
  the framework and a runtime.
- [Framework API](Framework-API.md): `filc_async.h`, the program API, the
  descriptor layout and the stub entry points.
- [The io_uring runtime](io_uring-Runtime.md): the shipped runtime, a worked
  example of the runtime API.
- [The RPC runtime example](RPC-Runtime.md): the second shipped runtime,
  which sends calls to a TCP server.
- [Explicit API](Explicit-API.md): `fasync.h`, the hand-written io_uring
  request API.
- [Tests](Testing.md): the test suite and what each check proves.

## Explanation

- [Architecture](Architecture.md): how an annotated call travels from the
  compiler through the framework to a runtime.
- [Demos and performance](Performance.md): what the demos show and the measured
  results.
- [Limitations](Limitations.md): what is not supported yet.
- [Glossary](Glossary.md).
