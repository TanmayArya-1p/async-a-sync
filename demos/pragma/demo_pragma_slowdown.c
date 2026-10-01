/* demo_pragma_slowdown: annotating code that cannot overlap makes it slower.
 *
 * One file holds a chain of records, in shuffled order. Each record's `next`
 * field is the offset of the record after it, so the only way to find a
 * record is to read the one before it. Three ways to read the whole chain:
 *
 *   A  chain_blocking   pread a record, read its `next`, pread that one
 *   B  chain_annotated  the same loop calling async_pread. `next` is needed
 *                       straight after each call, so every read is sent and
 *                       waited for on its own and nothing overlaps: B does
 *                       A's work plus the cost of the async machinery
 *   C  known_annotated  the same calls, but with the offsets known up front,
 *                       so all of them are in flight before the first `next`
 *                       is read
 *
 * An annotation only helps when the code has independent work to overlap, and
 * only when there is device latency to hide. From a warm page cache every
 * read is a memory copy, so even C pays for the machinery and gains nothing.
 *
 * Each way returns the sum of the `next` fields it read, so pragma_report.hh
 * can check that all three read the same bytes. It times each way from a warm
 * and from a dropped page cache and prints the medians.
 *
 * Usage: demo_pragma_slowdown [dir] [records] [passes] */

#include "pragma_report.hh"

long chain_blocking(int n) {
  long at = first_record, sum = 0;
  for (int i = 0; i < n; i++) {
    pread(chain_fd, records[i], sizeof(struct record), at);
    at = records[i]->next;
    sum += at;
  }
  return sum;
}

long chain_annotated(int n) {
  long at = first_record, sum = 0;
  for (int i = 0; i < n; i++) {
    async_pread(chain_fd, records[i], sizeof(struct record), at);
    at = records[i]->next; /* waits for the read just issued */
    sum += at;
  }
  return sum;
}

long known_annotated(int n) {
  for (int i = 0; i < n; i++)
    async_pread(chain_fd, records[i], sizeof(struct record), chain_offset[i]);

  long sum = 0;
  for (int i = 0; i < n; i++)
    sum += records[i]->next; /* the reads are already in flight */
  return sum;
}

int main(int argc, char** argv) {
  struct chain c = chain_setup(argc, argv);

  for (int pass = 0; pass < c.passes; pass++)
    for (int cold = 0; cold <= 1; cold++) {
      chain_time(&c, pass, cold, CHAIN_BLOCKING, chain_blocking);
      chain_time(&c, pass, cold, CHAIN_ANNOTATED, chain_annotated);
      chain_time(&c, pass, cold, KNOWN_ANNOTATED, known_annotated);
    }

  return chain_report(&c);
}
