/*
 * agentc mbedTLS configuration - freestanding TLS 1.2/1.3 client, X.509 only.
 *
 * This is a deliberately minimal hand-written alternative to mbedTLS'
 * include/mbedtls/mbedtls_config.h.  Everything not needed by a client that
 * performs a TLS 1.2/1.3 handshake against an embedded Mozilla CA bundle is
 * left undefined (mbedTLS treats undefined options as disabled).
 *
 * Build glue (see third_party/mbedtls_glue.c):
 *   - calloc/free are wired to agentc_alloc/agentc_free at runtime via
 *     mbedtls_platform_set_calloc_free();
 *   - time() is wired to os_now_ns(CLOCK_REALTIME) via
 *     mbedtls_platform_set_time();
 *   - snprintf/vsnprintf are provided by the glue via
 *     mbedtls_platform_set_snprintf()/_vsnprintf();
 *   - gmtime_r is provided by the glue (MBEDTLS_PLATFORM_GMTIME_R_ALT);
 *   - entropy is the platform os_random, see
 *     mbedtls_hardware_poll() in the glue.
 *
 * No libc, no filesystem, no networking module, no server, no DTLS.
 */
#ifndef MBEDTLS_AGENTC_CONFIG_H
#define MBEDTLS_AGENTC_CONFIG_H

/* ------------------------------------------------------------------ platform */
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS /* never include a libc header */
#define MBEDTLS_PLATFORM_MS_TIME_TYPE_MACRO long long
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY             /* runtime calloc/free hooks   */
#define MBEDTLS_PLATFORM_TIME_ALT           /* mbedtls_platform_set_time() */
#define MBEDTLS_PLATFORM_SNPRINTF_ALT
#define MBEDTLS_PLATFORM_VSNPRINTF_ALT
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_PLATFORM_ZEROIZE_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE              /* X.509 notBefore/notAfter    */

/* ------------------------------------------------------------------ hashes */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_ERROR_C                    /* mbedtls_strerror() text      */

/* ------------------------------------------------------ symmetric crypto */
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C

/* --------------------------------------------------------------- entropy */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ENTROPY_HARDWARE_ALT        /* mbedtls_hardware_poll (glue) */
#define MBEDTLS_ENTROPY_SHA512_ACCUMULATOR
#define MBEDTLS_NO_PLATFORM_ENTROPY         /* do not pull in open()/read() */

/* ------------------------------------------------------- bignum / pubkey */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15                   /* RSA PKCS#1 v1.5 (TLS 1.2)   */
#define MBEDTLS_PKCS1_V21                   /* RSA-PSS (TLS 1.3)          */

/* ------------------------------------------------------------ encoding */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C

/* ------------------------------------------------------------ PK / X.509 */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT

/* ------------------------------------------------------------- TLS stack */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
/* TLS 1.3 requires the peer certificate to be kept (mbedTLS check_config). */
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

/*
 * mbedTLS 3.6 implements the TLS 1.3 key schedule and ephemeral key exchange
 * on top of PSA Crypto.  TLS 1.3 cannot be enabled without the software PSA
 * core, so this is the one intentional deviation from "disable PSA": the
 * alternative would be to drop MBEDTLS_SSL_PROTO_TLS1_3 entirely.
 */
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_KEY_STORE_DYNAMIC
#define MBEDTLS_HKDF_C                       /* PSA HKDF for TLS 1.3      */

#endif /* MBEDTLS_AGENTC_CONFIG_H */
