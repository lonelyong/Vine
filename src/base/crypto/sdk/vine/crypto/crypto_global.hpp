#pragma once

#include <vine/vn_global.hpp>

#ifdef VN_CRYPTO_LIB
#    define VN_CRYPTO_API VN_EXPORT
#else
#    define VN_CRYPTO_API VN_IMPORT
#endif

/**
 * @brief vn::crypto provides cryptographic algorithms only.
 *
 * @details
 * - Available today:
 *   - digests: `Digest`, `digest()`, `Hasher`;
 *   - MACs: `hmac()`, `Hmac`, `macEquals()`;
 *   - key derivation: `Hkdf`; password hashing: `Pbkdf2`, `Argon2`;
 *   - ciphers: `AesGcm` (AEAD, the default), `AesCbc`, `AesCtr` (interoperability only);
 *   - asymmetric: `RsaKey` (PSS / PKCS#1 v1.5, OAEP, DER and PEM), `EccKey` (P-256 / P-384 ECDSA and ECDH);
 *   - `Random` for secure random bytes.
 * - Key management, certificates and policy belong to higher layers (appfw): this layer never decides what to trust,
 *   only what an algorithm computes. Certificate *parsing* is the one gap left; the rest is in `.ai/backlog/crypto.md`.
 * - The algorithms are wolfSSL's; this module owns the vocabulary, the error contract and the incremental shapes, not
 *   the primitives.
 */
#define VN_CRYPTO_NS_BEGIN                                                                                                                                     \
    namespace VN_ROOT_NS                                                                                                                                       \
    {                                                                                                                                                          \
    namespace crypto                                                                                                                                           \
    {

#define VN_CRYPTO_NS_END                                                                                                                                       \
    }                                                                                                                                                          \
    }
