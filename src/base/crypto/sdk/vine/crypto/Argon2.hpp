#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "crypto_global.hpp"

VN_CRYPTO_NS_BEGIN

/**
 * @brief Argon2 (RFC 9106): the recommended password hash.
 *
 * @details
 * - Argon2 is memory-hard: an attacker cannot trade memory for parallel guesses, which is what makes it preferable to
 *   PBKDF2 (see `Pbkdf2`) for anything new.
 * - `parallelism` is the number of lanes; 1 is a valid, simple choice. `memory` is the working set in KiB, and it is
 *   the parameter that matters most: raising it is what costs an attacker.
 * - `salt` must be unique per password, 16 bytes or more. Derive, then store the parameters and compare with
 *   `macEquals()` when verifying.
 *
 * @warning
 * - `memory * 1024` bytes are allocated per call. Choose it against the deployment target (tens of MiB for a desktop
 *   application, more if the machine can spare it) and never lower it to make a test fast.
 * - This is deliberately slow: calling it on the UI thread will be felt. Run it on a worker.
 */
class VN_CRYPTO_API Argon2 {
  public:
    /** @brief The Argon2 variants; `kId` is the one to use. */
    enum class Variant
    {
        kD,   ///< Data-dependent addressing: fastest, weakest against side channels.
        kI,   ///< Data-independent addressing: side-channel friendly, weaker against time-memory trade-offs.
        kId,  ///< Hybrid, RFC 9106's recommendation and this module's default.
    };

    /**
     * @brief Derives key material from a password.
     *
     * @param variant     The Argon2 variant; `kId` unless a format says otherwise.
     * @param password    The password bytes (UTF-8 for text).
     * @param salt        The per-credential salt; at least 8 bytes, 16 or more recommended.
     * @param parallelism Number of lanes; must be at least 1.
     * @param memory      Working set in KiB; must be at least 8 × `parallelism`.
     * @param iterations  Number of passes; at least 1.
     * @param length      Number of output bytes; must be at least 4.
     * @return The derived bytes, `length` long.
     * @throws std::invalid_argument when a parameter is out of range.
     * @throws std::runtime_error    when the backend cannot allocate or rejects the input.
     */
    static std::vector<std::byte> derive(Variant variant, std::span<const std::byte> password,
                                        std::span<const std::byte> salt, std::uint32_t parallelism,
                                        std::uint32_t memory, std::uint32_t iterations, std::size_t length);
};

VN_CRYPTO_NS_END
