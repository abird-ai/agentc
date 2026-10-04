/* tls_shim.h — private interface of the Linux mbedTLS TLS backend.
 *
 * Public entry points are declared in include/net.h; this header only adds
 * the poll-direction accessor the HTTP loop needs. The backend is built by
 * the frozen build.sh with the ordinary freestanding CFLAGS, so the mbedTLS
 * headers are reached through the src/mbedtls and src/psa symlinks (see the
 * report) and MBEDTLS_CONFIG_FILE is defined here.
 */
#ifndef AGENTC_TLS_SHIM_H
#define AGENTC_TLS_SHIM_H

#include "net.h"

/* Direction to poll after agentc_tls_handshake/read/write returned -EAGAIN:
 * 0 none, 1 wait readable, 2 wait writable. */
int agentc_tls_want(const AgcTls *t);

#endif /* AGENTC_TLS_SHIM_H */
