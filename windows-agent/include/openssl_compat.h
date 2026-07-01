/**
 * @file openssl_compat.h
 * @brief OpenSSL 1.0.2 (legacy profile) ↔ 1.1+/3.x API shims.
 *
 * The legacy build (PROFILE=legacy) links OpenSSL 1.0.2u, which predates the
 * EVP_MD_CTX_new / EVP_MD_CTX_free names (1.0.2 used create / destroy). Include
 * this AFTER <openssl/evp.h> in any TU that uses the digest-context API so the
 * same source compiles against both 1.0.2 and 1.1+/3.x.
 */
#ifndef AGENT_OPENSSL_COMPAT_H
#define AGENT_OPENSSL_COMPAT_H

#include <openssl/opensslv.h>

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define EVP_MD_CTX_new   EVP_MD_CTX_create
#define EVP_MD_CTX_free  EVP_MD_CTX_destroy
#endif

#endif /* AGENT_OPENSSL_COMPAT_H */
