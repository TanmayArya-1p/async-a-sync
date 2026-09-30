#pragma once

/* The title, checks and verdict every annotated-call demo prints. */

#include <stdio.h>
#include <string.h>

/* Checks are recorded as a demo runs. pragma_finish prints one line for all
 * of them and names only the ones that failed, so a passing run stays short. */
#define PRAGMA_MAX_CHECKS 32
static const char* pragma_check_what[PRAGMA_MAX_CHECKS];
static int pragma_check_ok[PRAGMA_MAX_CHECKS];
static int pragma_nchecks;
static int pragma_failures;

static inline void pragma_check(const char* what, int ok) {
  if (pragma_nchecks < PRAGMA_MAX_CHECKS) {
    pragma_check_what[pragma_nchecks] = what;
    pragma_check_ok[pragma_nchecks] = ok;
    pragma_nchecks++;
  }
  if (!ok)
    pragma_failures++;
}

static inline void pragma_title(const char* title, const char* subtitle) {
  printf("\n%s\n", title);
  for (size_t i = 0; i < strlen(title); i++)
    putchar('-');
  printf("\n");
  if (subtitle)
    printf("%s\n\n", subtitle);
}

static inline int pragma_finish(void) {
  printf("  checks: %d/%d passed\n", pragma_nchecks - pragma_failures,
         pragma_nchecks);
  for (int i = 0; i < pragma_nchecks; i++)
    if (!pragma_check_ok[i])
      printf("    FAILED: %s\n", pragma_check_what[i]);
  printf("\n%s\n", pragma_failures ? "DEMO FAILED" : "DEMO OK");
  return pragma_failures ? 1 : 0;
}
