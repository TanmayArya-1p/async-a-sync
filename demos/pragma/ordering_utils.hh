#pragma once

// Scenarios, setup and report for demo_pragma_ordering.

#include "pragma_utils.hh"

enum { FILES = 8, RECORD = 16 };

// What a scenario's calls should do while they are issued.
enum { BATCHES, WAITS };

struct demo {
  int fd[FILES];         // the files the writes go to
  int records;           // a file of FILES numbered records
  char* record[FILES];   // one buffer per read
  char path[FILES + 1][256];
};

struct scenario {
  const char* what;
  int calls;
  int waited;           // a call waited before its body ran
  unsigned long submits; // io_uring_enter calls that submitted
  int ok;
};

static struct {
  int n;
  int first_call; // the first logged call of the next scenario
  struct counters start;
  struct scenario row[4];
} scenarios __attribute__((unused));

// Fills fd with numbered records for the reads scenario.
static inline void write_records(int fd) {
  char record[RECORD + 1];
  for (int i = 0; i < FILES; i++) {
    snprintf(record, sizeof(record), "record number %02d", i);
    if (pwrite(fd, record, RECORD, (off_t)i * RECORD) != RECORD) {
      perror("write_records");
      exit(1);
    }
  }
}

static inline int is_record(const char* buf, int i) {
  char record[RECORD + 1];
  snprintf(record, sizeof(record), "record number %02d", i);
  return equal(buf, record, RECORD);
}

static inline struct demo setup(int argc, char** argv) {
  struct demo d = {0};
  title("ordering: dependencies order calls, the rest batch",
        "  Calls that conflict on an fd wait; independent calls batch.");
  for (int i = 0; i < FILES; i++) {
    char name[64];
    snprintf(name, sizeof(name), "demo_pragma_ordering_%d.dat", i);
    d.fd[i] = make_file(d.path[i], sizeof(d.path[i]), dir_arg(argc, argv),
                        name, "");
    d.record[i] = (char*)calloc(1, RECORD + 1);
  }
  d.records = make_file(d.path[FILES], sizeof(d.path[FILES]), dir_arg(argc, argv),
                        "demo_pragma_ordering_records.dat", "");
  write_records(d.records);
  warm_up(d.fd[0]);
  scenarios.start = counters_now();
  return d;
}

// Records a scenario from the calls logged since the last one.
static inline void scenario(const char* what, int ok, int expect) {
  struct scenario* s = &scenarios.row[scenarios.n++];
  struct counters used = counters_since(scenarios.start);
  int first = scenarios.first_call;
  int last = calls.n < LOG_MAX ? calls.n : LOG_MAX;
  s->what = what;
  s->calls = calls.n - first;
  s->waited = last > first && calls.call[last - 1].entries > calls.call[first].entries;
  s->submits = used.submits;
  s->ok = ok;

  check(what, ok);
  if (expect == WAITS) {
    check("  conflicting calls waited inside the call", s->waited);
  } else {
    check("  independent calls did not wait while issued", !s->waited);
    check("  the batch reached the kernel in one submit", s->submits == 1);
  }
  scenarios.first_call = calls.n;
  scenarios.start = counters_now();
}

static inline int report(struct demo* d) {
  // waited: a call entered the kernel before its body ran, to wait for the
  // conflicting call before it.
  printf("  %-30s %5s %7s %8s   %s\n", "scenario", "calls", "waited",
         "submits", "outcome");
  for (int i = 0; i < scenarios.n; i++) {
    struct scenario* s = &scenarios.row[i];
    printf("  %-30s %5d %7s %8lu   %s\n", s->what, s->calls,
           s->waited ? "yes" : "no", s->submits, s->ok ? "ok" : "FAILED");
  }
  printf("\n  => conflicting calls kept their order; %d independent calls, "
         "%lu kernel submit\n",
         scenarios.row[1].calls, scenarios.row[1].submits);

  for (int i = 0; i < FILES; i++) {
    close(d->fd[i]);
    unlink(d->path[i]);
    free(d->record[i]);
  }
  close(d->records);
  unlink(d->path[FILES]);
  check_bodies();
  return finish();
}
