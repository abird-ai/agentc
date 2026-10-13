/* mcp_live_stubs.c — replay-backend stubs for the real-network MCP live driver.
 *
 * tests/mcp_test.c is built twice: the golden binary links src/net/mock.c and
 * runs every offline test; build/mcp_stream_live links the real network backend
 * so its --live-http-stream* modes can drive tests/httpd.py. Only those modes
 * run in the live binary, but the linked-in offline tests still reference the
 * replay entry points, so the three symbols they call are stubbed here. Calling
 * one at runtime is a bug in the harness, never a silent pass.
 */
#include "base/limits.h"
#include "agentc.h"
#include "net/net_internal.h"

int agentc_mock_load(const char *path) {
    (void)path;
    return -38; /* ENOSYS */
}

void agentc_mock_reset(void) {}

const AgcBuf *agentc_mock_sent(void) {
    static const AgcBuf empty;
    return &empty;
}
