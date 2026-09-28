/* tests/t_pending_open_failure.c -- closing a pending descriptor whose open
 * failed must still free its handle.
 *
 * fasync_open_pending hands out one of 64 negative handles. fasync_close on a
 * handle whose open failed has no descriptor to close; it reports the open's
 * error, but the handle has to go away too, or 64 failed opens use up the
 * table for the rest of the process.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fasync.h"

#define OPENS 200

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char missing[512];
    if (snprintf(missing, sizeof missing, "%s/t_pending_open_failure_%ld.missing",
                 dir, (long)getpid()) >= (int)sizeof missing)
        return 1;
    unlink(missing);

    int handles_ok = 1;
    int close_ok = 1;
    int first_bad = -1;
    for (int i = 0; i < OPENS; i++) {
        int h = fasync_open_pending(AT_FDCWD, missing, O_RDONLY, 0);
        if (h > -2) {
            if (first_bad < 0)
                first_bad = i;
            handles_ok = 0;
            break;
        }
        errno = 0;
        /* the failed open has no descriptor to close: the close reports it */
        if (fasync_close(h) != 0 || errno != ENOENT)
            close_ok = 0;
    }

    /* a handle whose open succeeded still closes normally afterwards */
    int h = fasync_open_pending(AT_FDCWD, dir, O_RDONLY | O_DIRECTORY, 0);
    fasync_id c = h <= -2 ? fasync_close(h) : 0;
    int good_ok = c && fasync_result(c) == 0;

    int ok = handles_ok && close_ok && good_ok;
    printf("T_PENDING_OPEN_FAILURE %s (handles=%d first_bad=%d close=%d good=%d)\n",
           ok ? "PASS" : "FAIL", handles_ok, first_bad, close_ok, good_ok);
    return ok ? 0 : 1;
}
