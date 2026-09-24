#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_CRYPTO_LIB
#    define VN_CRYPTO_API VN_EXPORT
#else
#    define VN_CRYPTO_API VN_IMPORT
#endif

/**
 * @brief vn::crypto provides cryptographic algorithms only.
 *
 * Scope: hashes (MD5/SHA-1/SHA-256/CRC32), ciphers (AES/RSA/ChaCha20) and
 * HMAC. Key management and certificates belong to higher layers (appfw).
 */
#define VN_CRYPTO_NS_BEGIN                                                                                                                               \
    namespace VN_ROOT_NS                                                                                                                                 \
    {                                                                                                                                                   \
    namespace crypto                                                                                                                                    \
    {

#define VN_CRYPTO_NS_END                                                                                                                                 \
    }                                                                                                                                                   \
    }
