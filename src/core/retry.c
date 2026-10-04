/* retry.c — retry/backoff policy for the agent loop.
 *
 * Retryable: transport failures (negative errno), HTTP 408/409/429 and 5xx
 * (the transport returns the status code), and a stream dropped before any
 * content. Backoff is 500 ms * 2^n capped at 60 s, overridden by Retry-After.
 */
#include "agentc.h"

#define RETRY_BASE_MS 500
#define RETRY_CAP_MS 60000

i64 agentc_retry_backoff_ms(int attempt, i64 retry_after_ms) {
    if (retry_after_ms > 0) return retry_after_ms > RETRY_CAP_MS ? RETRY_CAP_MS : retry_after_ms;
    i64 ms = RETRY_BASE_MS;
    for (int i = 0; i < attempt && ms < RETRY_CAP_MS; i++) ms *= 2;
    return ms > RETRY_CAP_MS ? RETRY_CAP_MS : ms;
}

bool agentc_retry_retryable(int rc) {
    /* out of memory, permission, cancelled and ENOSYS are not transient */
    if (rc == -12 || rc == -13 || rc == -125 || rc == -38) return false;
    if (rc > 0) {
        if (rc == 408 || rc == 409 || rc == 429) return true;
        return rc >= 500 && rc <= 599;
    }
    if (rc < 0) {
        /* EINVAL/ENOSYS/EOPNOTSUPP are configuration or protocol errors. */
        if (rc == -22 || rc == -38 || rc == -95) return false;
        return true;
    }
    return false;
}
