#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "crypto_global.hpp"
#include "Digest.hpp"

VN_CRYPTO_NS_BEGIN

/**
 * @brief PBKDF2 (RFC 8018): a slow, salted, iterated hash for passwords.
 *
 * @details
 * - **Use `Argon2` for new passwords.** PBKDF2 exists because formats and peers already specify it (PKCS#8 key
 *   encryption, older credential stores); it is not memory-hard, so an attacker with a GPU gets far more guesses per
 *   second than against Argon2.
 * - `password` is the UTF-8 encoded password, `salt` a per-credential random value, and `iterations` the work
 *   factor; salt and iteration count travel beside the derived key.
 * - The salt is only required to be non-empty, because formats in the wild use short salts (RFC 6070's own vectors use
 *   four bytes). RFC 8018 recommends at least eight octets and this module recommends sixteen or more.
 * - Verification is not provided: store the parameters, derive again, and compare with `macEquals()`.
 *
 * @warning
 * - A low iteration count is a silent weakness. Pick it from a measurement on the deployment target, not from a
 *   constant copied out of a tutorial, and raise it as hardware improves.
 * - The salt must be unique per password; reusing one lets an attacker share work across accounts.
 */
class VN_CRYPTO_API Pbkdf2 {
  public:
    /**
     * @brief Derives key material from a password.
     *
     * @param algorithm  The digest to drive the iterations with; SHA-256 is the usual choice.
     * @param password   The password bytes.
     * @param salt       The per-credential salt; must not be empty, 16 or more bytes recommended.
     * @param iterations Work factor; must be at least 1.
     * @param length     Number of output bytes; must be at least 1.
     * @return The derived bytes, `length` long.
     * @throws std::invalid_argument when a size or the iteration count is out of range.
     * @throws std::runtime_error    when the backend rejects the input.
     */
    static std::vector<std::byte> derive(Digest algorithm, std::span<const std::byte> password,
                                        std::span<const std::byte> salt, std::uint32_t iterations,
                                        std::size_t length);
};

VN_CRYPTO_NS_END
