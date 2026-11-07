#pragma once

#include <array>
#include <cstddef>

#include <wolfssl/wolfcrypt/types.h>

#include <vine/crypto/Digest.hpp>

VN_CRYPTO_NS_BEGIN

namespace detail
{

/// Storage wide enough for the largest backend hash context (`wc_Sha3`); checked by a static_assert in the source.
inline constexpr std::size_t kDigestContextSize = 512;

/**
 * @brief Backing storage for one backend hash context.
 *
 * @details
 * - wolfSSL exposes a different context type per algorithm. This type owns the bytes and the source file casts them
 *   to the right context, which is what lets one implementation serve both the one-shot and the incremental shape.
 *
 * @warning
 * - Treat it as opaque: only `digestInit()` / `digestUpdate()` / `digestFinal()` / `digestFree()` may touch it.
 */
struct DigestContext
{
    alignas(16) std::array<std::byte, kDigestContextSize> storage{};
};

/**
 * @brief Maps a digest to the backend's hash type id.
 *
 * @param digest The digest.
 * @return The wolfSSL hash type, for example `WC_HASH_TYPE_SHA256`.
 */
enum wc_HashType wolfHashType(Digest digest);

/**
 * @brief Initializes a context for the given digest.
 *
 * @param digest  The digest.
 * @param context The context to initialize.
 * @return True on success.
 */
bool digestInit(Digest digest, DigestContext& context);

/**
 * @brief Feeds bytes into an initialized context.
 *
 * @param digest  The digest the context was initialized for.
 * @param context The context.
 * @param data    First input byte; may be null when `size` is zero.
 * @param size    Number of input bytes; arbitrary, chunked internally to the backend's 32-bit length.
 * @return True on success.
 */
bool digestUpdate(Digest digest, DigestContext& context, const void* data, std::size_t size);

/**
 * @brief Writes the digest into `out` and releases the context's resources.
 *
 * @param digest  The digest the context was initialized for.
 * @param context The context; unusable afterwards.
 * @param out     Buffer of at least `digestSize(digest)` bytes.
 * @return True on success.
 */
bool digestFinal(Digest digest, DigestContext& context, unsigned char* out);

/**
 * @brief Releases a context that will not be finalized.
 *
 * @param digest  The digest the context was initialized for.
 * @param context The context; unusable afterwards.
 */
void digestFree(Digest digest, DigestContext& context);

} // namespace detail

VN_CRYPTO_NS_END
