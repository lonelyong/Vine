#pragma once

#include "crypto_global.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

VN_CRYPTO_NS_BEGIN

/**
 * @brief The digests this module can compute.
 *
 * @details
 * - This enum is the single spelling of a digest in `vn::crypto`: the one-shot function, the incremental `Hasher`,
 *   HMAC, RSA and ECC all select the algorithm through it, so "which hash" is answered in exactly one way.
 * - Values are plain enumerators (not tied to a backend's numbering): the mapping to wolfSSL lives in one function.
 * - Names are accepted as `sha256`, `SHA-256`, `sha3_512` — case, dashes and underscores do not matter.
 */
enum class Digest
{
    kMd5,      ///< MD5: 16 bytes. Legacy interop only; broken for any adversarial use.
    kSha1,     ///< SHA-1: 20 bytes. Legacy interop only.
    kSha224,   ///< SHA-2 224: 28 bytes.
    kSha256,   ///< SHA-2 256: 32 bytes. The default for signatures and fingerprints.
    kSha384,   ///< SHA-2 384: 48 bytes.
    kSha512,   ///< SHA-2 512: 64 bytes.
    kSha3_224, ///< SHA-3 224: 28 bytes.
    kSha3_256, ///< SHA-3 256: 32 bytes.
    kSha3_384, ///< SHA-3 384: 48 bytes.
    kSha3_512, ///< SHA-3 512: 64 bytes.
};

/**
 * @brief Returns the digest length in bytes.
 *
 * @param digest The digest.
 * @return 16 for MD5 up to 64 for SHA-512 / SHA3-512.
 */
std::size_t VN_CRYPTO_API digestSize(Digest digest);

/**
 * @brief Returns the canonical lower-case name of a digest.
 *
 * @param digest The digest.
 * @return The name without separators, for example `u8"sha3_512"`; it is a compile-time constant, not a copy.
 */
std::u8string_view VN_CRYPTO_API digestName(Digest digest);

/**
 * @brief Resolves a digest from its name.
 *
 * @details
 * - Case, `-` and `_` are ignored, so `"SHA-256"`, `"sha256"` and `"sha_256"` all mean the same digest.
 *
 * @param name The caller's spelling.
 * @return The digest.
 * @throws std::invalid_argument when the name is unknown; there is no silent fallback to a default digest.
 */
Digest VN_CRYPTO_API digestFromName(std::u8string_view name);

/**
 * @brief Computes the digest of a byte range in one call.
 *
 * @details
 * - Use this for data already in memory; `Hasher` is the incremental form for data that arrives in chunks.
 * - The result is a `std::vector` because the length depends on the digest: check `digestSize(digest)` when a
 *   fixed-size buffer is needed.
 *
 * @param digest The digest to compute.
 * @param data   The input bytes; an empty range is valid and hashes as the empty message.
 * @return The digest bytes, `digestSize(digest)` long.
 * @throws std::runtime_error when the backend cannot compute it.
 */
std::vector<std::byte> VN_CRYPTO_API digest(Digest digest, std::span<const std::byte> data);

VN_CRYPTO_NS_END
