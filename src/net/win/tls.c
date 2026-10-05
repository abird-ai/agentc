/* tls.c — Windows AgcTls backend on SChannel (secur32) + crypt32.
 *
 * SChannel is a token-passing state machine, so InitializeSecurityContextW is
 * driven by agentc_tls_handshake() and returns -EAGAIN with agentc_tls_want() whenever
 * the underlying non-blocking socket needs more data. Certificates are
 * validated manually (SCH_CRED_MANUAL_CRED_VALIDATION): after the handshake we
 * pull SECPKG_ATTR_REMOTE_CERT_CONTEXT, build a chain with
 * CertGetCertificateChain and run CERT_CHAIN_POLICY_SSL with the hostname via
 * CertVerifyCertificateChainPolicy. Clearing AGENTC_TLS_VERIFY skips both the
 * break and the policy check (--insecure), exactly like mbedTLS on Linux.
 *
 * Records: EncryptMessage produces header+payload+trailer in one scratch
 * buffer; DecryptMessage consumes buffered raw bytes and keeps SECBUFFER_EXTRA
 * for the next call. Minimum protocol is TLS 1.2 (TLS 1.3 is enabled too when
 * the OS understands the credential bit).
 *
 * No windows.h: all SecBuffer/SCHANNEL_CRED/SecPkgContext layouts are declared
 * in this file and the functions imported from secur32/crypt32.
 */
#include "net/net_internal.h"
#include "plat/win/win.h"

#define TLS_EAGAIN 11
#define TLS_EACCES 13
#define TLS_EIO 5
#define TLS_EINVAL 22
#define TLS_EPIPE 32
#define TLS_ECONNRESET 104

/* ------------------------------------------------------------- SSPI types */
typedef struct {
    WinULONG_PTR dwLower;
    WinULONG_PTR dwUpper;
} WinSecHandle;

typedef struct {
    u64 LowPart;
    i64 HighPart;
} WinTimeStamp;

typedef struct {
    WinDWORD cbBuffer;
    WinDWORD BufferType;
    void *pvBuffer;
} WinSecBuffer;

typedef struct {
    WinDWORD ulVersion;
    WinDWORD cBuffers;
    WinSecBuffer *pBuffers;
} WinSecBufferDesc;

typedef struct {
    WinDWORD dwVersion;
    WinDWORD dwCredFormat;
    WinDWORD cCreds;
    void **paCred;
    void *hRootStore;
    WinDWORD cMappers;
    void **aphMappers;
    WinDWORD cSupportedAlgs;
    void *palgSupportedAlgs;
    WinDWORD grbitEnabledProtocols;
    WinDWORD dwMinimumCipherStrength;
    WinDWORD dwMaximumCipherStrength;
    WinDWORD dwSessionLifespan;
    WinDWORD dwFlags;
} WinSchannelCred;

typedef struct {
    WinDWORD cbHeader;
    WinDWORD cbTrailer;
    WinDWORD cbMaximumMessage;
    WinDWORD cBuffers;
    WinDWORD cbBlockSize;
} WinStreamSizes;

typedef struct {
    WinDWORD cbSize;
    WinDWORD dwAuthType;
    WinDWORD fdwChecks;
    u16 *pwszServerName;
} WinSslExtraPara;

typedef struct {
    WinDWORD cbSize;
    WinDWORD dwFlags;
    void *pvExtraPolicyPara;
} WinChainPolicyPara;

typedef struct {
    WinDWORD cbSize;
    WinDWORD dwError;
    WinLONG lChainIndex;
    WinLONG lElementIndex;
    void *pvExtraPolicyStatus;
} WinChainPolicyStatus;

typedef struct {
    WinDWORD cUsageIdentifier;
    WinLPSTR *rgpszUsageIdentifier;
} WinEnhkeyUsage;

typedef struct {
    WinDWORD dwType;
    WinEnhkeyUsage Usage;
} WinCertUsageMatch;

typedef struct {
    WinDWORD cbSize;
    WinCertUsageMatch RequestedUsage;
    WinCertUsageMatch RequestedIssuancePolicy;
    WinDWORD dwUrlRetrievalTimeout;
    WinBOOL fCheckRevocationFreshnessTime;
    WinDWORD dwRevocationFreshnessTime;
    WinFILETIME *pftCacheResync;
    void *pStrongSignPara;
} WinCertChainPara;

__declspec(dllimport) int AcquireCredentialsHandleW(
    u16 *pszPrincipal, u16 *pszPackage, WinDWORD fCredentialUse, void *pvLogonID,
    void *pAuthData, void *pGetKeyFn, void *pvGetKeyArgument,
    WinSecHandle *phCredential, WinTimeStamp *ptsExpiry);
__declspec(dllimport) int FreeCredentialsHandle(WinSecHandle *phCredential);
__declspec(dllimport) int InitializeSecurityContextW(
    WinSecHandle *phCredential, WinSecHandle *phContext, u16 *pszTargetName,
    WinDWORD fContextReq, WinDWORD Reserved1, WinDWORD TargetDataRep,
    WinSecBufferDesc *pInput, WinDWORD Reserved2, WinSecHandle *phNewContext,
    WinSecBufferDesc *pOutput, WinDWORD *pfContextAttr, WinTimeStamp *ptsExpiry);
__declspec(dllimport) int DeleteSecurityContext(WinSecHandle *phContext);
__declspec(dllimport) int QueryContextAttributesW(WinSecHandle *phContext,
                                                  WinDWORD ulAttribute, void *pBuffer);
__declspec(dllimport) int EncryptMessage(WinSecHandle *phContext, WinDWORD fQOP,
                                         WinSecBufferDesc *pMessage,
                                         WinDWORD MessageSeqNo);
__declspec(dllimport) int DecryptMessage(WinSecHandle *phContext,
                                         WinSecBufferDesc *pMessage,
                                         WinDWORD MessageSeqNo, WinDWORD *pfQOP);

__declspec(dllimport) WinBOOL CertGetCertificateChain(
    void *hChainEngine, void *pCertContext, WinFILETIME *pTime,
    void *hAdditionalStore, void *pChainPara, WinDWORD dwFlags, void *pvReserved,
    void **ppChainContext);
__declspec(dllimport) WinBOOL CertVerifyCertificateChainPolicy(
    WinLPCSTR pszPolicyOID, void *pChainContext, void *pPolicyPara,
    void *pPolicyStatus);
__declspec(dllimport) void CertFreeCertificateChain(void *pChainContext);
__declspec(dllimport) WinBOOL CertFreeCertificateContext(void *pCertContext);

/* ------------------------------------------------------------- constants */
#define SEC_E_OK_ 0
#define SEC_I_CONTINUE_NEEDED_ 0x00090312
#define SEC_I_INCOMPLETE_CREDENTIALS_ 0x00090320
#define SEC_I_CONTEXT_EXPIRED_ 0x00090317
#define SEC_I_RENEGOTIATE_ 0x00090321
#define SEC_E_INCOMPLETE_MESSAGE_ 0x80090318
#define SEC_E_INVALID_HANDLE_ 0x80090301
#define SEC_E_INVALID_TOKEN_ 0x80090308
#define SEC_E_UNTRUSTED_ROOT_ 0x80090325
#define SEC_E_WRONG_PRINCIPAL_ 0x80090322
#define SEC_E_CERT_EXPIRED_ 0x80090328
#define SEC_E_CERT_UNKNOWN_ 0x80090327
#define SEC_E_ILLEGAL_MESSAGE_ 0x80090326
#define SEC_E_MESSAGE_ALTERED_ 0x8009030F
#define SEC_E_OUT_OF_SEQUENCE_ 0x80090310
#define SEC_E_ENCRYPT_FAILURE_ 0x80090329
#define SEC_E_DECRYPT_FAILURE_ 0x80090330
#define SEC_E_BUFFER_TOO_SMALL_ 0x80090321
#define SEC_E_TIME_SKEW_ 0x80090324
#define SEC_E_ALGORITHM_MISMATCH_ 0x80090331
#define SEC_E_STRONG_CRYPTO_NOT_SUPPORTED_ 0x8009033A
#define SEC_E_DOWNGRADE_DETECTED_ 0x80090350
#define SEC_E_UNSUPPORTED_FUNCTION_ 0x80090302

#define SECPKG_CRED_OUTBOUND_ 2
#define SECPKG_ATTR_STREAM_SIZES_ 4
#define SECPKG_ATTR_REMOTE_CERT_CONTEXT_ 0x53

#define SCHANNEL_CRED_VERSION_ 4
#define SCH_CRED_MANUAL_CRED_VALIDATION_ 0x00000008
#define SCH_CRED_NO_DEFAULT_CREDS_ 0x00000010
#define SCH_USE_STRONG_CRYPTO_ 0x00400000
#define SP_PROT_TLS1_2_CLIENT_ 0x00000800
#define SP_PROT_TLS1_3_CLIENT_ 0x00002000

#define ISC_REQ_REPLAY_DETECT_ 0x00000004
#define ISC_REQ_SEQUENCE_DETECT_ 0x00000008
#define ISC_REQ_CONFIDENTIALITY_ 0x00000010
#define ISC_REQ_EXTENDED_ERROR_ 0x00004000
#define ISC_REQ_STREAM_ 0x00008000

#define SECBUFFER_EMPTY_ 0
#define SECBUFFER_DATA_ 1
#define SECBUFFER_TOKEN_ 2
#define SECBUFFER_EXTRA_ 5
#define SECBUFFER_STREAM_TRAILER_ 6
#define SECBUFFER_STREAM_HEADER_ 7
#define SECBUFFER_VERSION_ 0
#define SECBUFFER_MISSING_ 4

#define CERT_CHAIN_POLICY_SSL_ ((WinLPCSTR)(uintptr_t)4)
#define AUTHTYPE_SERVER_ 2

/* ------------------------------------------------------------------ state */
struct AgcTls {
    int fd;
    int want;
    int net_errno;
    bool verify;
    bool handshake_done;
    bool hs_started;
    bool have_cred;
    bool have_ctx;
    WinSecHandle cred;
    WinSecHandle ctx;
    WinStreamSizes sizes;
    u16 *target; /* SNI / hostname check, NULL when AGENTC_TLS_NO_SNI */
    u8 *in;
    size_t in_len, in_cap;
    u8 *dec;
    size_t dec_off, dec_len, dec_cap;
    u8 *wr;
    size_t wr_len, wr_off, wr_cap;
    size_t wr_plain; /* plaintext bytes encrypted into the pending wr record */
    char err[256];
};

static char g_last_error[256];

static void tls_set_err(AgcTls *t, const char *msg) {
    agentc_snprintf(t->err, sizeof t->err, "%s", msg);
}

static void tls_set_status(AgcTls *t, int status) {
    agentc_snprintf(t->err, sizeof t->err, "TLS: SChannel error 0x%08x", (unsigned)status);
}

/* -------------------------------------------------------------- IO helpers */
static int tls_flush(AgcTls *t) {
    while (t->wr_off < t->wr_len) {
        int n = agentc_net_send(t->fd, t->wr + t->wr_off, t->wr_len - t->wr_off);
        if (n == -TLS_EAGAIN) {
            t->want = 2;
            return -TLS_EAGAIN;
        }
        if (n < 0) return n;
        if (n == 0) return -TLS_ECONNRESET;
        t->wr_off += (size_t)n;
    }
    t->wr_len = 0;
    t->wr_off = 0;
    return 0;
}

/* Read raw bytes into the handshake/receive buffer. */
static int tls_fill_input(AgcTls *t) {
    if (t->in_len == t->in_cap) {
        size_t cap = t->in_cap ? t->in_cap * 2 : 32768;
        if (cap > 262144) return -TLS_EIO;
        t->in = agentc_realloc(t->in, cap);
        t->in_cap = cap;
    }
    int n = agentc_net_recv(t->fd, t->in + t->in_len, t->in_cap - t->in_len);
    if (n == -TLS_EAGAIN) {
        t->want = 1;
        return -TLS_EAGAIN;
    }
    if (n < 0) return n;
    if (n == 0) return t->in_len == 0 ? 0 : -TLS_ECONNRESET;
    t->in_len += (size_t)n;
    return 0;
}

static void tls_in_consume(AgcTls *t, size_t n) {
    if (n >= t->in_len) {
        t->in_len = 0;
        return;
    }
    agentc_memmove(t->in, t->in + n, t->in_len - n);
    t->in_len -= n;
}

/* Handshake input: a clean EOF while SChannel still expects a token is a
 * truncated handshake (wrong port, proxy, middlebox), not "more data please".
 * Returning 0 here would make the handshake loop call recv() forever in a
 * 100% CPU spin. */
static int tls_fill_handshake(AgcTls *t) {
    int rc = tls_fill_input(t);
    if (rc == 0 && t->in_len == 0) return -TLS_ECONNRESET;
    return rc;
}

/* ------------------------------------------------------------ verification */
static bool verify_cert(AgcTls *t) {
    void *cert = NULL;
    if (QueryContextAttributesW(&t->ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT_, &cert) !=
            SEC_E_OK_ ||
        cert == NULL) {
        tls_set_err(t, "TLS: remote certificate unavailable");
        return false;
    }
    WinCertChainPara para;
    agentc_memset(&para, 0, sizeof para);
    para.cbSize = sizeof para;
    void *chain = NULL;
    if (!CertGetCertificateChain(NULL, cert, NULL, NULL, &para, 0, NULL, &chain) ||
        chain == NULL) {
        agentc_snprintf(t->err, sizeof t->err,
                    "TLS: certificate chain build failed (0x%08x)",
                    (unsigned)GetLastError());
        CertFreeCertificateContext(cert);
        return false;
    }
    WinSslExtraPara extra;
    agentc_memset(&extra, 0, sizeof extra);
    extra.cbSize = sizeof extra;
    extra.dwAuthType = AUTHTYPE_SERVER_;
    extra.pwszServerName = t->target; /* NULL: chain-only, no hostname check */
    WinChainPolicyPara pp;
    agentc_memset(&pp, 0, sizeof pp);
    pp.cbSize = sizeof pp;
    pp.pvExtraPolicyPara = &extra;
    WinChainPolicyStatus ps;
    agentc_memset(&ps, 0, sizeof ps);
    ps.cbSize = sizeof ps;
    WinBOOL ok = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL_, chain, &pp,
                                                   &ps);
    bool good = ok && ps.dwError == 0;
    if (!good) {
        const char *why = "certificate verification failed";
        if (t->target == NULL) why = "certificate chain verification failed";
        agentc_snprintf(t->err, sizeof t->err, "TLS: %s (0x%08x)", why,
                    (unsigned)ps.dwError);
    }
    CertFreeCertificateChain(chain);
    CertFreeCertificateContext(cert);
    return good;
}

/* ---------------------------------------------------------------- public API */
AgcTls *agentc_tls_new(int fd, const char *host, u16 port, unsigned flags) {
    (void)port;
    if (fd < 0) return NULL;
    g_last_error[0] = '\0';

    AgcTls *t = agentc_alloc(sizeof *t);
    t->fd = fd;
    t->verify = (flags & AGENTC_TLS_VERIFY) != 0;
    if (host != NULL && host[0] != '\0' && !(flags & AGENTC_TLS_NO_SNI)) {
        t->target = win_utf8_to_wide(host);
        /* Verification keys off the SNI/target name; a host that cannot be
         * converted must fail closed instead of silently skipping it. */
        if (t->target == NULL && t->verify) {
            agentc_snprintf(g_last_error, sizeof g_last_error,
                        "TLS: host name conversion failed");
            agentc_free(t);
            return NULL;
        }
    }

    static const u16 package[] = u"Microsoft Unified Security Protocol Provider";
    WinSchannelCred cred;
    agentc_memset(&cred, 0, sizeof cred);
    cred.dwVersion = SCHANNEL_CRED_VERSION_;
    cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT_ | SP_PROT_TLS1_3_CLIENT_;
    cred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION_ | SCH_CRED_NO_DEFAULT_CREDS_ |
                   SCH_USE_STRONG_CRYPTO_;
    WinTimeStamp expiry;
    int r = AcquireCredentialsHandleW(NULL, (u16 *)package, SECPKG_CRED_OUTBOUND_,
                                       NULL, &cred, NULL, NULL, &t->cred, &expiry);
    if (r != SEC_E_OK_) {
        /* Pre-1809 Windows does not know TLS 1.3; retry with 1.2 only. */
        cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT_;
        r = AcquireCredentialsHandleW(NULL, (u16 *)package, SECPKG_CRED_OUTBOUND_,
                                       NULL, &cred, NULL, NULL, &t->cred, &expiry);
    }
    if (r != SEC_E_OK_) {
        agentc_snprintf(g_last_error, sizeof g_last_error,
                    "TLS: AcquireCredentialsHandle failed (0x%08x)", (unsigned)r);
        if (t->target != NULL) agentc_free(t->target);
        agentc_free(t);
        return NULL;
    }
    t->have_cred = true;
    return t;
}

int agentc_tls_handshake(AgcTls *t) {
    if (t == NULL || !t->have_cred) return -TLS_EINVAL;
    for (;;) {
        t->want = 0;
        t->net_errno = 0;
        if (t->handshake_done) return 0;
        if (t->wr_len > t->wr_off) {
            int rc = tls_flush(t);
            if (rc < 0) return rc;
            continue;
        }
        if (!t->hs_started || t->in_len > 0) {
            /* ready to feed a token (first call needs no input) */
        } else {
            int rc = tls_fill_handshake(t);
            if (rc < 0) return rc;
            continue;
        }

        WinSecBuffer inbuf;
        WinSecBufferDesc indesc;
        bool have_input = t->in_len > 0;
        if (have_input) {
            inbuf.cbBuffer = (WinDWORD)t->in_len;
            inbuf.BufferType = SECBUFFER_TOKEN_;
            inbuf.pvBuffer = t->in;
            indesc.ulVersion = SECBUFFER_VERSION_;
            indesc.cBuffers = 1;
            indesc.pBuffers = &inbuf;
        }
        if (t->wr == NULL) {
            t->wr = agentc_alloc(65536);
            t->wr_cap = 65536;
        }
        WinSecBuffer outbuf;
        outbuf.cbBuffer = (WinDWORD)t->wr_cap;
        outbuf.BufferType = SECBUFFER_TOKEN_;
        outbuf.pvBuffer = t->wr;
        WinSecBufferDesc outdesc;
        outdesc.ulVersion = SECBUFFER_VERSION_;
        outdesc.cBuffers = 1;
        outdesc.pBuffers = &outbuf;

        WinDWORD attrs = 0;
        WinTimeStamp expiry;
        int r = InitializeSecurityContextW(
            &t->cred, t->hs_started ? &t->ctx : NULL, t->target,
            ISC_REQ_REPLAY_DETECT_ | ISC_REQ_SEQUENCE_DETECT_ |
                ISC_REQ_CONFIDENTIALITY_ | ISC_REQ_EXTENDED_ERROR_ | ISC_REQ_STREAM_,
            0, 0, have_input ? &indesc : NULL, 0, t->hs_started ? NULL : &t->ctx,
            &outdesc, &attrs, &expiry);
        t->hs_started = true;

        if (r == SEC_I_INCOMPLETE_CREDENTIALS_) {
            tls_set_err(t, "TLS: server requested a client certificate");
            return -TLS_EACCES;
        }
        if (r == SEC_E_INCOMPLETE_MESSAGE_) {
            int rc = tls_fill_handshake(t); /* keep the partial token buffered */
            if (rc == 0) continue;
            return rc;
        }
        if (r == SEC_I_CONTINUE_NEEDED_ || r == SEC_E_OK_) {
            if (have_input) tls_in_consume(t, t->in_len);
            if (outbuf.cbBuffer > 0) {
                t->wr_len = outbuf.cbBuffer;
                t->wr_off = 0;
                int rc = tls_flush(t);
                if (rc < 0) return rc;
            }
            if (r == SEC_E_OK_) {
                if (QueryContextAttributesW(&t->ctx, SECPKG_ATTR_STREAM_SIZES_,
                                             &t->sizes) != SEC_E_OK_) {
                    tls_set_err(t, "TLS: SECPKG_ATTR_STREAM_SIZES failed");
                    return -TLS_EIO;
                }
                if (t->verify && !verify_cert(t)) return -TLS_EACCES;
                t->handshake_done = true;
                return 0;
            }
            continue; /* SEC_I_CONTINUE_NEEDED: token sent, read the reply */
        }
        if (r == SEC_E_INVALID_HANDLE_) {
            tls_set_err(t, "TLS: invalid SChannel context");
            return -TLS_EINVAL;
        }
        if (r == SEC_E_UNTRUSTED_ROOT_ || r == SEC_E_WRONG_PRINCIPAL_ ||
            r == SEC_E_CERT_EXPIRED_ || r == SEC_E_CERT_UNKNOWN_ ||
            r == SEC_E_TIME_SKEW_) {
            tls_set_status(t, r);
            return -TLS_EACCES;
        }
        tls_set_status(t, r);
        return -TLS_EIO;
    }
}

int agentc_tls_read(AgcTls *t, void *p, size_t n) {
    if (t == NULL || !t->handshake_done) return -TLS_EINVAL;
    t->want = 0;
    t->net_errno = 0;
    if (t->dec_len > 0) {
        size_t take = t->dec_len < n ? t->dec_len : n;
        agentc_memcpy(p, t->dec + t->dec_off, take);
        t->dec_off += take;
        t->dec_len -= take;
        if (t->dec_len == 0) t->dec_off = 0;
        return (int)take;
    }
    for (;;) {
        if (t->in_len > 0) {
            WinSecBuffer bufs[4];
            agentc_memset(bufs, 0, sizeof bufs);
            bufs[0].cbBuffer = (WinDWORD)t->in_len;
            bufs[0].BufferType = SECBUFFER_DATA_;
            bufs[0].pvBuffer = t->in;
            bufs[1].BufferType = SECBUFFER_EMPTY_;
            bufs[2].BufferType = SECBUFFER_EMPTY_;
            bufs[3].BufferType = SECBUFFER_EMPTY_;
            WinSecBufferDesc desc;
            desc.ulVersion = SECBUFFER_VERSION_;
            desc.cBuffers = 4;
            desc.pBuffers = bufs;
            WinDWORD qop = 0;
            int r = DecryptMessage(&t->ctx, &desc, 0, &qop);
            if (r == SEC_E_INCOMPLETE_MESSAGE_) {
                /* fall through to read more */
            } else if (r == SEC_E_OK_ || r == SEC_I_CONTEXT_EXPIRED_ ||
                       r == SEC_I_RENEGOTIATE_) {
                size_t extra = 0;
                void *extra_ptr = NULL;
                size_t written = 0;
                u8 *dst = p;
                size_t room = n;
                for (int i = 0; i < 4; i++) {
                    if (bufs[i].BufferType == SECBUFFER_DATA_ && bufs[i].pvBuffer != NULL) {
                        size_t l = bufs[i].cbBuffer;
                        size_t take = l < room ? l : room;
                        agentc_memcpy(dst, bufs[i].pvBuffer, take);
                        dst += take;
                        room -= take;
                        written += take;
                        if (l > take) {
                            size_t rest = l - take;
                            if (t->dec_len + rest > t->dec_cap) {
                                t->dec = agentc_realloc(t->dec, t->dec_len + rest);
                                t->dec_cap = t->dec_len + rest;
                            }
                            agentc_memcpy(t->dec + t->dec_len,
                                      (const u8 *)bufs[i].pvBuffer + take, rest);
                            t->dec_len += rest;
                        }
                    } else if (bufs[i].BufferType == SECBUFFER_EXTRA_) {
                        extra = bufs[i].cbBuffer;
                        extra_ptr = bufs[i].pvBuffer;
                    }
                }
                if (extra > 0 && extra_ptr != NULL) {
                    agentc_memmove(t->in, extra_ptr, extra);
                    t->in_len = extra;
                } else {
                    t->in_len = 0;
                }
                if (written > 0) return (int)written;
                if (r == SEC_I_CONTEXT_EXPIRED_) return 0;
                if (r == SEC_I_RENEGOTIATE_) {
                    tls_set_err(t, "TLS: peer-initiated renegotiation is not supported");
                    return -TLS_EIO;
                }
                continue; /* control record consumed */
            } else if (r == SEC_E_INVALID_HANDLE_) {
                tls_set_err(t, "TLS: invalid SChannel context");
                return -TLS_EINVAL;
            } else if (r == SEC_E_MESSAGE_ALTERED_ || r == SEC_E_OUT_OF_SEQUENCE_ ||
                       r == SEC_E_DECRYPT_FAILURE_) {
                tls_set_status(t, r);
                return -TLS_ECONNRESET;
            } else if (r == SEC_E_INVALID_TOKEN_) {
                tls_set_status(t, r);
                return -TLS_ECONNRESET;
            } else {
                tls_set_status(t, r);
                return -TLS_EIO;
            }
        }
        int rc = tls_fill_input(t);
        if (rc < 0) return rc;
        if (rc == 0 && t->in_len == 0) return 0; /* clean EOF */
    }
}

int agentc_tls_write(AgcTls *t, const void *p, size_t n) {
    if (t == NULL || !t->handshake_done) return -TLS_EINVAL;
    t->want = 0;
    t->net_errno = 0;
    if (t->sizes.cbMaximumMessage == 0) return -TLS_EIO;

    const u8 *src = p;
    size_t left = n;
    size_t accepted = 0; /* plaintext bytes whose records are on the wire */

    /* Invariant: a queued record holds the first wr_plain bytes of the
     * caller's buffer. After -EAGAIN (or a short return) wire/http.c retries
     * exactly those bytes, so flush the record and skip them instead of
     * encrypting them a second time. Bytes are never counted as accepted until
     * their record has been fully written, so a caller that stops calling
     * cannot leave the tail of a request stuck in a buffer. */
    if (t->wr_len > t->wr_off) {
        if (left < t->wr_plain) return -TLS_EINVAL; /* caller did not retry the queued chunk */
        int rc = tls_flush(t);
        if (rc < 0) return rc;
        accepted += t->wr_plain;
        src += t->wr_plain;
        left -= t->wr_plain;
        t->wr_plain = 0;
    }
    if (accepted == n) return (int)accepted;

    for (;;) {
        size_t chunk = left;
        if (chunk > t->sizes.cbMaximumMessage) chunk = t->sizes.cbMaximumMessage;
        size_t total = t->sizes.cbHeader + chunk + t->sizes.cbTrailer;
        if (t->wr == NULL || total > t->wr_cap) {
            t->wr = agentc_realloc(t->wr, total);
            t->wr_cap = total;
        }
        agentc_memcpy(t->wr + t->sizes.cbHeader, src, chunk);
        WinSecBuffer bufs[4];
        agentc_memset(bufs, 0, sizeof bufs);
        bufs[0].cbBuffer = t->sizes.cbHeader;
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER_;
        bufs[0].pvBuffer = t->wr;
        bufs[1].cbBuffer = (WinDWORD)chunk;
        bufs[1].BufferType = SECBUFFER_DATA_;
        bufs[1].pvBuffer = t->wr + t->sizes.cbHeader;
        bufs[2].cbBuffer = t->sizes.cbTrailer;
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER_;
        bufs[2].pvBuffer = t->wr + t->sizes.cbHeader + chunk;
        bufs[3].BufferType = SECBUFFER_EMPTY_;
        WinSecBufferDesc desc;
        desc.ulVersion = SECBUFFER_VERSION_;
        desc.cBuffers = 4;
        desc.pBuffers = bufs;
        int r = EncryptMessage(&t->ctx, 0, &desc, 0);
        if (r != SEC_E_OK_) {
            tls_set_status(t, r);
            return accepted > 0 ? (int)accepted : -TLS_EIO;
        }
        t->wr_len = (size_t)bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
        t->wr_off = 0;
        t->wr_plain = chunk;
        int rc = tls_flush(t);
        if (rc == -TLS_EAGAIN) {
            /* The record is queued but not on the wire: do not claim its
             * plaintext. The caller retries this chunk on the next call. */
            return accepted > 0 ? (int)accepted : rc;
        }
        if (rc < 0) {
            /* Hard failure: the record can never complete and the connection
             * is unusable, so drop it rather than leave it for a retry. */
            t->wr_len = 0;
            t->wr_off = 0;
            t->wr_plain = 0;
            return accepted > 0 ? (int)accepted : rc;
        }
        accepted += chunk;
        src += chunk;
        left -= chunk;
        t->wr_plain = 0;
        if (left == 0) return (int)accepted;
    }
}

void agentc_tls_close(AgcTls *t) {
    if (t == NULL) return;
    if (t->have_ctx) DeleteSecurityContext(&t->ctx);
    if (t->have_cred) FreeCredentialsHandle(&t->cred);
    if (t->target != NULL) agentc_free(t->target);
    if (t->in != NULL) agentc_free(t->in);
    if (t->dec != NULL) agentc_free(t->dec);
    if (t->wr != NULL) agentc_free(t->wr);
    agentc_free(t);
}

const char *agentc_tls_error(AgcTls *t) {
    if (t != NULL && t->err[0] != '\0') return t->err;
    if (g_last_error[0] != '\0') return g_last_error;
    return "";
}

int agentc_tls_want(const AgcTls *t) { return t == NULL ? 0 : t->want; }
