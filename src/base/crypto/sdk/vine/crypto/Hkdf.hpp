#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "crypto_global.hpp"
#include "Digest.hpp"

VN_CRYPTO_NS_BEGIN

/**
 * @brief HKDF (RFC 5869): turns shared key material into usable keys.
 *
 * @details
 * - `hkdf()` is extract-then-expand in one call, the form almost every caller wants; the two halves are exposed
 *   separately for protocols that fix the PRK (for example when the salt is chosen by a standard).
 * - Use it on anything that is "key material" rather than a key: an ECDH shared secret, a password hash output, or a
 *   master key that has to produce several purpose-bound keys.
 * - `info` binds the output to a purpose; two different `info` values under one PRK give unrelated keys.
 *
 * @warning
 * - HKDF is not a password hash: it is fast and unsalted by design, so a password goes through a slow KDF instead.
 * - The output length is bounded by `255 * digestSize(algorithm)` (RFC 5869); asking for more throws.
 * - Without a salt, use a zero-filled one of `digestSize(algorithm)` bytes — never skip the extract step for that
 *   reason alone, because extract is what mixes the input's structure away.
 */
class VN_CRYPTO_API Hkdf {
  public:
    /**
     * @brief Extracts a pseudo-random key from input key material.
     *
     * @details
     * - This is `HMAC(salt, ikm)`: it collapses input with structure (a Diffie-Hellman point, a concatenated secret)
     *   into a uniform-looking key.
     *
     * @param algorithm The digest to use inside HMAC.
     * @param salt      Non-secret randomizer; may be empty (RFC 5869 then uses a zero-filled salt).
     * @param ikm       The input key material.
     * @return The PRK, `digestSize(algorithm)` bytes long.
     * @throws std::runtime_error when the backend rejects the input.
     */
    static std::vector<std::byte> extract(Digest algorithm, std::span<const std::byte> salt,
                                          std::span<const std::byte> ikm);

    /**
     * @brief Expands a pseudo-random key into output key material.
     *
     * @param algorithm The digest to use; must be the one `extract()` used.
     * @param prk       The pseudo-random key from `extract()`.
     * @param info      Context and purpose binding; may be empty.
     * @param length    Number of output bytes; must be 1..`255 * digestSize(algorithm)`.
     * @return The output key material, `length` bytes.
     * @throws std::invalid_argument when `length` is out of range.
     * @throws std::runtime_error    when the backend rejects the input.
     */
    static std::vector<std::byte> expand(Digest algorithm, std::span<const std::byte> prk,
                                         std::span<const std::byte> info, std::size_t length);

    /**
     * @brief Extracts and expands in one call.
     *
     * @param algorithm The digest to use inside HMAC.
     * @param ikm       The input key material.
     * @param salt      Non-secret randomizer; may be empty.
     * @param info      Context and purpose binding; may be empty.
     * @param length    Number of output bytes; must be 1..`255 * digestSize(algorithm)`.
     * @return The output key material, `length` bytes.
     * @throws std::invalid_argument when `length` is out of range.
     * @throws std::runtime_error    when the backend rejects the input.
     */
    static std::vector<std::byte> derive(Digest algorithm, std::span<const std::byte> ikm,
                                        std::span<const std::byte> salt, std::span<const std::byte> info,
                                        std::size_t length);
};

VN_CRYPTO_NS_END
