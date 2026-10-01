# Prior art

This page collects older ideas that look like the ones in async-a-sync. For
each, it gives a short explanation in plain words and says where it is like
this project and where it differs. Links for further reading are at the end.

## What async-a-sync does, in one paragraph

You mark a C function with a pragma. When the program calls it, the call is
handed to a "runtime" (for example io_uring for file reads, or a TCP server)
and returns right away. Any buffer the call will fill is flagged as "not ready
yet". The first time the program touches that buffer, it waits until the call
has finished. The program never writes "submit" or "wait". Calls can also say
what they read and write (`r_dep`, `w_dep`), so calls that touch the same
thing still run in the right order.

That breaks into a few separate ideas, and each has a history:

| Idea in async-a-sync | Closest older idea |
|---|---|
| A call returns a result that is not ready yet | Futures and promises |
| Touching the result waits for it automatically | Implicit futures (MultiLisp), dataflow variables (Oz) |
| A "not ready" flag on the memory itself | Full/empty bits (Tera MTA), I-structures (Id) |
| The compiler adds a check before every memory access | Garbage collector read barriers (ZGC), Fil-C itself |
| Waiting on first touch of memory | Demand paging, `userfaultfd` |
| Declaring what each call reads and writes | OpenMP `depend`, OmpSs `in`/`out` |
| Sending many I/O requests in one batch | io_uring, FlexSC |
| Asynchronous remote calls | Liskov's promises, Cap'n Proto |
| Code that looks blocking but runs asynchronously | Virtual threads, Zig's new I/O, the "function color" debate |

## 1. Futures and promises

**What it is.** A *future* (or *promise*) is a placeholder for a value that
is still being computed. You start some work, get the placeholder back at
once, and collect the real value later.

**MultiLisp (1985)** made futures *implicit*. You wrote `(future expr)` to
start work, but there was no special "get the value" step: any operation
that needed the real value, such as adding it to a number, simply waited
for it. MultiLisp called this waiting "touching" the future.

**Most modern languages use explicit futures.** Java's `Future.get()`,
JavaScript's `await`, and C++'s `std::future::get()` all make you ask for
the value on purpose.

**How it compares.** async-a-sync is closest to MultiLisp: there is no
"get" step. Two differences:
- The placeholder here is not a special value. It is the caller's own
  buffer, flagged as not ready.
- MultiLisp could check every value because it was a dynamic language. C has
  no such check, so this project adds one through the compiler (see
  section 3).

## 2. Memory that waits until it is filled

**Dataflow variables (Oz, and the older dataflow languages).** A variable
starts out empty and can be set only once. Any thread that reads it before
it is set simply pauses until another thread sets it.

**I-structures (the Id language).** These are the same idea for arrays. Each
element is "empty" or "full", and reading an empty element waits until
someone writes it.

**Full/empty bits (the HEP and Tera MTA computers).** These did the same in
hardware. Every word of memory carried one extra bit saying whether it held
real data. A read could be told to wait until the bit said "full".

**How it compares.** This is the nearest match to async-a-sync's "pending
flag". Each Fil-C object has a header, and this project uses one spare bit
in it to mean "a call is still filling this". Reading an object with the
bit set waits, much like reading an empty word on the Tera MTA. Differences:
- It is done in software, per object rather than per word.
- The flag is set by a call on its way out and cleared when the call
  finishes, not by an ordinary write.

## 3. A check the compiler puts before every access

**Read barriers in garbage collectors.** Some garbage collectors move
objects while the program is running. To keep the program from following a
stale pointer, the compiler inserts a tiny check wherever a pointer is
loaded. The check is almost always a quick "all good" (the *fast path*).
Only rarely does it drop into slower code to fix things up (the *slow
path*). Java's ZGC is a well-known recent example.

**Fil-C.** This project is built on Fil-C, a memory-safe version of C. Fil-C
already gives every pointer an invisible link to its object's header
("InvisiCaps") and checks accesses against it.

**How it compares.** async-a-sync adds one more check of the same shape.
Before an access, it tests the pending bit in the object's header; that is
the cheap fast path. Only if the bit is set does it call into the runtime to
wait; that is the slow path. It is a read barrier whose job is waiting for
I/O instead of helping a garbage collector. The design depends on Fil-C: in
plain C there is no header to put the bit in.

## 4. Waiting on first touch, at the operating-system level

**Demand paging and `mmap`.** When you map a file into memory, the operating
system does not read it right away. The first time the program touches a
page, the processor stops the program (a *page fault*), the kernel reads
that page, and the program continues as if nothing happened.

**`userfaultfd`.** This Linux feature lets a normal program handle those
page faults itself. It is used, for example, to restore virtual machine
snapshots lazily, loading memory only when it is first touched.

**How it compares.** From the program's point of view, the feeling is the
same: memory that looks ready, where the first touch quietly waits.
Differences:
- The operating system works on whole pages (usually 4 KiB), and can only
  wait on memory it controls (files, swap).
- async-a-sync works per object and can wait on anything a runtime does,
  such as a network reply.
- It also starts the work early, when the call is made, instead of at the
  first touch.

## 5. Declaring what each task reads and writes

**OpenMP tasks with `depend`.** In OpenMP you can mark a block of code as a
task and say what it reads (`depend(in: x)`) and what it writes
(`depend(out: x)`). The runtime then runs tasks in parallel where it is safe
and in order where two tasks touch the same data and one of them writes.

**OmpSs** (from the Barcelona Supercomputing Center) goes further and puts
`in`/`out` annotations on *function declarations*, so that every call to
that function becomes a task with those dependencies.

**How it compares.** This is very close to async-a-sync's `r_dep` and
`w_dep`:
- Both put the annotation on the function, as OmpSs does.
- Both let readers run together and make a writer wait, like a
  readers-writer lock.
- Differences: OpenMP and OmpSs are about running *computation* in parallel
  on many cores. async-a-sync is about letting *I/O and remote calls*
  overlap with the rest of the program. Here, a call that must wait blocks
  inside the call, instead of being queued in a task graph.

## 6. Start now, wait later: Cilk

**What it is.** Cilk adds two keywords to C: `cilk_spawn` in front of a call
lets it run in parallel, and `cilk_sync` waits for all spawned calls in the
current function.

**How it compares.** It has the same "start the call, keep going" shape.
The difference is that Cilk needs an explicit `cilk_sync`. async-a-sync has
no sync point: each result waits only when it is touched.

## 7. Batching requests to the kernel

**io_uring (Linux, 2019).** Programs put I/O requests into a shared queue and
tell the kernel about many of them with a single system call. Results come
back through a second queue. This saves the cost of entering the kernel once
per request.

**FlexSC (2010, research).** An earlier research system with the same goal:
programs write system-call requests into shared memory, keep running, and
check later whether they are done.

**Older Linux AIO** also let programs start reads and collect them later,
but it was awkward to use and only worked well for some kinds of files.

**How it compares.** The io_uring runtime in this project uses io_uring
underneath. Its twist is *when* the batch goes to the kernel: calls only
queue requests, and the whole queue is sent the first time any result is
needed. The program gets the batching without writing any io_uring code.

## 8. Asynchronous remote calls

**Promises in Argus (Liskov and Shrira, 1988).** This is where the word
"promise" comes from. A remote call returned a promise at once, and several
calls to the same server could be sent together in order (a "call-stream").
The program "claimed" the promise when it needed the answer, waiting if it
was not back yet.

**Cap'n Proto and promise pipelining.** Cap'n Proto can send a call that
uses the result of an earlier call *before* that result has come back. The
server fills it in, so a chain of dependent calls costs about one network
round trip instead of one per call.

**How it compares.** The rpc runtime in this project (`runtime=rpc`) is a
small version of Liskov's promises:
- Calls return at once, and reading the reply waits.
- Calls marked as touching the same counter keep their order.
- Unlike Cap'n Proto, there is no pipelining: a call that depends on another
  waits for it on the client side before being sent.

## 9. Hiding I/O waits without the programmer's help

**SpecHint (Chang and Gibson, 1999).** While a program was stuck waiting on
the disk, SpecHint ran a copy of it ahead to guess which files it would read
next. It then told the file system to start reading them early. The
program's code was not changed at all.

**How it compares.** Both aim to hide I/O waits without rewriting the
program in an asynchronous style. SpecHint *guesses* future reads.
async-a-sync *knows* them, because the annotated call says exactly what to
read, and it starts the read at the call instead of guessing.

## 10. "What color is your function?"

**The problem.** In a 2015 blog post, Bob Nystrom pointed out that in
languages with `async`/`await`, functions come in two "colors". An async
function can only be awaited from another async function, so the async
color spreads up through every caller.

**Answers from other languages:**
- **Go and Java's virtual threads** let code block normally. The runtime
  quietly parks the lightweight thread and runs something else meanwhile.
  The code looks synchronous, but each blocking call still waits right away.
- **Zig's new I/O design (2025)** passes an I/O interface into functions,
  so the same function can run synchronously or asynchronously depending on
  what the caller provides.

**How it compares.** async-a-sync is another answer to the same problem.
Annotated functions have no color: callers look exactly like ordinary C, and
no caller has to change. Unlike virtual threads, it does not park a thread
at the call. The call returns, the program keeps running, and the wait
happens later, at the first touch, often after many other requests have
been started.

## What seems new here

As far as this review found, each piece above exists somewhere. The
combination looks new:
- Implicit, MultiLisp-style waiting in *C*.
- Done per object through Fil-C's memory-safety headers, rather than in
  hardware or at page granularity.
- With OmpSs-style read/write annotations on the function.
- With the "how" left to pluggable runtimes, one per function.

This is a reading of the sources below, not an exhaustive literature search.

## Further reading

Short posts and articles first, then a few papers for the curious.

**Futures, promises and function color**
- [What Color is Your Function?](https://journal.stuffwithstuff.com/2015/02/01/what-color-is-your-function/) (Bob Nystrom): the function-color problem.
- [Futures and Promises](https://www.cs.utexas.edu/~rossbach/cs380p/papers/Futures.html): a readable history, from MultiLisp's implicit futures through promise pipelining.
- [Promises](https://christophermeiklejohn.com/pl/2016/03/04/promises.html) (Christopher Meiklejohn): short summary of Liskov and Shrira's promises and call-streams.
- [Futures and Eventual Values, Part 1](https://davidbkemp.blogspot.com/2005/03/futures-and-eventual-values-part-1.html) (David Kemp): a gentle introduction to futures.

**Memory that waits until filled**
- [scala-dataflow README](https://github.com/jboner/scala-dataflow): Oz-style dataflow variables with small examples.
- [The Tera MTA](https://www.netlib.org/utk/papers/advanced-computers/tera.html): a few paragraphs, including its full/empty bits.
- [Id (programming language)](https://en.wikipedia.org/wiki/Id_(programming_language)): short page covering I-structures.

**Compiler-inserted checks and Fil-C**
- [Fil-C: A memory-safe C implementation](https://lwn.net/Articles/1042938/) (LWN): overview of the project this one builds on.
- [InvisiCaps by example](https://github.com/pizlonator/fil-c/blob/deluge/invisicaps_by_example.md): how Fil-C pointers find their object headers.
- [Deep-dive of ZGC's Architecture](https://dev.java/learn/jvm/tool/garbage-collection/zgc-deepdive/): see its short "Load Barriers" section for the fast-path/slow-path idea.

**Waiting on first touch**
- [Linux Page Faults, mmap, and userfaultfd](https://www.shayon.dev/post/2026/65/linux-page-faults-mmap-and-userfaultfd/): demand paging and user-space fault handling, explained step by step.

**Declaring reads and writes**
- [OpenMP task basics, part 2](https://hpc2n.github.io/Task-based-parallelism/branch/master/task-basics-2/) (HPC2N): `depend(in/out)` with small examples.
- [OmpSs programming model](https://pm.bsc.es/ftp/ompss/doc/book/spec/programming_model.html): the `in`/`out` annotations on tasks and functions.
- [Programming in Cilk](https://cilk.mit.edu/programming/): `cilk_spawn` and `cilk_sync`.

**Batching I/O and system calls**
- [Ringing in a new asynchronous I/O API](https://lwn.net/Articles/776703/) (LWN): the original introduction to io_uring.
- [FlexSC summary](https://stopire.github.io/2021/12/25/FlexSC-Flexible-System-Call-Scheduling-with-Exception-Less-System-Calls/index.html): a short write-up of the FlexSC paper.

**Remote calls**
- [Promise Pipelining and Dependent Calls](https://capnproto.org/news/2013-12-13-promise-pipelining-capnproto-vs-ice.html) (Cap'n Proto): what pipelining buys over plain async RPC.

**Blocking-looking code that doesn't block**
- [Project Loom's Virtual Threads: Why Blocking Code Is Cool Again](https://www.javacodegeeks.com/2026/02/project-looms-virtual-threads-why-blocking-code-is-cool-again.html): Java's virtual threads.
- [Zig's New Async I/O](https://kristoff.it/blog/zig-new-async-io/) (Loris Cro): one function, run sync or async by the caller's choice.

**Papers, for depth**
- [Automatic I/O Hint Generation through Speculative Execution](https://www.usenix.org/conference/osdi-99/automatic-io-hint-generation-through-speculative-execution) (SpecHint, OSDI 1999).
- [FlexSC: Flexible System Call Scheduling with Exception-Less System Calls](https://www.usenix.org/legacy/event/osdi10/tech/full_papers/Soares.pdf) (OSDI 2010).
