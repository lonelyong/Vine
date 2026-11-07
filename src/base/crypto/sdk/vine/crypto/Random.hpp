#pragma once

#include "crypto_global.hpp"

#include <cstddef>
#include <span>
#include <vector>

VN_CRYPTO_NS_BEGIN

/**
 * @brief Cryptographically secure random bytes.
 *
 * @details
 * - Every call draws from the platform entropy source through wolfSSL's DRBG; there is no user-visible state, no seed
 *   function and no way to make the output repeatable. A test that needs fixed bytes supplies them itself.
 * - One process-wide context is kept behind a mutex, so the calls are thread-safe and cheap after the first one. The
 *   first call is also the moment a failure becomes possible (no entropy source available).
 * - This is the only entry point in the module that produces nondeterminism: every other function takes its inputs
 *   from the caller, which is what makes them testable against published vectors.
 *
 * @warning
 * - `fill` throws when the entropy source fails. Do not catch and continue with partially filled bytes: the bytes that
 *   were written before the failure are not distinguishable from good ones.
 * - The output is for keys, nonces and salts. It is *not* a fast generator: do not use it for bulk data that is not
 *   secret (use a counter plus `AesGcm` for that).
 */
class VN_CRYPTO_API Random {
  public:
    /**
     * @brief Fills a byte span with random bytes.
     *
     * @param buffer The span to fill; an empty span is a no-op.
     * @throws std::runtime_error when the entropy source fails.
     */
    static void fill(std::span<std::byte> buffer);

    /**
     * @brief Fills a raw buffer with random bytes.
     *
     * @param data Pointer to the first byte to fill.
     * @param size Number of bytes to fill; zero is a no-op.
     * @throws std::runtime_error when the entropy source fails, or when @p data is null with a non-zero @p size.
     */
    static void fill(void* data, std::size_t size);

    /**
     * @brief Returns a freshly generated random byte vector.
     *
     * @param size Number of bytes to generate; zero returns an empty vector.
     * @return The random bytes.
     * @throws std::runtime_error when the entropy source fails.
     */
    static std::vector<std::byte> bytes(std::size_t size);
};

VN_CRYPTO_NS_END
