/* tests/t_dag_submit_failure.c -- fasync_run_dag reports ops it could not run.
 *
 * When submit fails for an op, that op and everything ordered after it never
 * execute. Returning success there would tell the caller its writes happened.
 * The submit function here always fails, so no io_uring is needed.
 */
#include <stdio.h>

#include "fasync_dep.h"

static fasync_id refuse(const struct fasync_op* op, void* ctx)
{
    (void)op;
    (void)ctx;
    return 0;
}

int main(void)
{
    char a[64];
    struct fasync_access w[] = { FASYNC_ACCESS_RANGE(a, sizeof(a), FASYNC_OUT) };
    struct fasync_access r[] = { FASYNC_ACCESS_RANGE(a, sizeof(a), FASYNC_IN) };
    struct fasync_op ops[] = { { "write", w, 1 }, { "read", r, 1 } };

    unsigned edges[4];
    unsigned n_edges = fasync_build_dag(ops, 2, edges, 4, 0);

    struct fasync_dag_run run = { 0 };
    int rc = fasync_run_dag(ops, 2, edges, n_edges, refuse, 0, &run);

    int ok = n_edges == 1 && rc == -1 && run.submitted == 0;
    printf("T_DAG_SUBMIT_FAILURE %s (edges=%u rc=%d submitted=%u)\n",
           ok ? "PASS" : "FAIL", n_edges, rc, run.submitted);
    return ok ? 0 : 1;
}
