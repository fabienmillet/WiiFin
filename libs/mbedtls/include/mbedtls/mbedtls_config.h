/**
 * mbedTLS minimal configuration for Nintendo Wii (devkitPPC, libogc)
 *
 * - No POSIX filesystem access
 * - No POSIX threading
 * - No built-in TCP/IP (replaced by libogc net_* in custom bio callbacks)
 * - TLS 1.2 only (sufficient for Jellyfin)
 * - AES-based cipher suites (Broadway/750 has no AES-NI but these are soft)
 */

#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

/* ---- System / Platform ---- */
/* No file system */
// #define MBEDTLS_FS_IO
/* No arc4random / getrandom — we provide entropy via libogc HW timer */
/* No POSIX net sockets — app supplies custom bio callbacks */
// #define MBEDTLS_NET_C

/* ---- Threading: none ---- */
// #define MBEDTLS_THREADING_C
// #define MBEDTLS_THREADING_PTHREAD

/* ---- TLS versions ---- */
#define MBEDTLS_SSL_PROTO_TLS1_2
// Keep TLS 1.3 disabled to reduce code size (no ticket/session required)
// #define MBEDTLS_SSL_PROTO_TLS1_3

/* ---- Cipher suites needed for Jellyfin (typically ECDHE-RSA-AES256-GCM-SHA384) ---- */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CCM_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_MODE_GCM
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SHA1_C       /* needed for cert fingerprints */
#define MBEDTLS_MD5_C        /* needed for some RSA ops */
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21

/* ---- ECC (for ECDHE key exchange) ---- */
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED  /* Cloudflare uses ECDSA certs */
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

/* ---- X.509 certificate handling ---- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PEM_PARSE_C   /* for PEM-format CA bundles */
#define MBEDTLS_BASE64_C      /* required for PEM */
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

/* ---- SSL/TLS core ---- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C     /* client only */
// #define MBEDTLS_SSL_SRV_C  /* no server */
#define MBEDTLS_SSL_SERVER_NAME_INDICATION  /* SNI */

/* ---- Misc crypto ---- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_MD_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_DHM_C
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* Bare-metal: no /dev/random or CryptGenRandom; we supply mbedtls_hardware_poll() */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_PLATFORM_C   /* mbedtls_platform_set_calloc_free etc. */

/* Memory — use standard malloc/free (newlib provides these on devkitPPC) */
// #define MBEDTLS_MEMORY_BUFFER_ALLOC_C

/* Padding schemes */
#define MBEDTLS_CIPHER_PADDING_PKCS7
#define MBEDTLS_CIPHER_PADDING_ONE_AND_ZEROS
#define MBEDTLS_CIPHER_PADDING_ZEROS

/* ---- Error strings (optional, saves ~10KB if disabled) ---- */
#define MBEDTLS_ERROR_C

/* ---- Debug (disable for release builds) ---- */
// #define MBEDTLS_DEBUG_C

/* Note: check_config.h is included by build_info.h after config_adjust_legacy_crypto.h
 * Do NOT include it here directly. */

#endif /* MBEDTLS_CONFIG_H */
