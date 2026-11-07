#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <vine/MemoryStream.hpp>

#include "crypto_global.hpp"
#include "Digest.hpp"

VN_CRYPTO_NS_BEGIN

/**
 * @brief Incremental digest calculation for a digest chosen at run time.
 *
 * @details
 * - Feed the input with `update()` (chunks of any size, including single bytes), then read the result once with
 *   `finish()`; the result equals `digest(Digest, span)` over the concatenation.
 * - The object owns one backend context, so it is move-only: copying would duplicate an in-progress computation.
 * - `finish()` may be called more than once and answers the same bytes; `update()` after it throws.
 *
 * @note
 * - The name follows the shape other libraries use for incremental hashing (RustCrypto's `Hasher`, Go's `hash.Hash`,
 *   libsodium's `crypto_hash_*_state`): one object per computation, verb methods, no `get` prefixes.
 *
 * @warning
 * - Not thread-safe: one hasher belongs to one producer.
 */
class VN_CRYPTO_API Hasher {
  public:
    /**
     * @brief Starts a calculation for the given digest.
     *
     * @param algorithm The digest to compute.
     * @throws std::runtime_error when the backend cannot initialize the context.
     */
    explicit Hasher(Digest algorithm);

    /**
     * @brief Starts a calculation, selecting the digest by name.
     *
     * @param name Digest name, as accepted by `digestFromName` (`"SHA-256"`, `"sha3_512"`, ...).
     * @throws std::invalid_argument when the name is unknown.
     * @throws std::runtime_error    when the backend cannot initialize the context.
     */
    explicit Hasher(std::u8string_view name);

    ~Hasher();

    /**
     * @brief Moves the in-progress context; the moved-from object must not be used again.
     *
     * @param other The object to move from.
     */
    Hasher(Hasher&& other) noexcept;

    Hasher& operator=(Hasher&& other) noexcept;

    Hasher(const Hasher&)            = delete;
    Hasher& operator=(const Hasher&) = delete;

    /**
     * @brief Feeds a byte range.
     *
     * @param data Pointer to the input bytes; may be null when `size` is zero.
     * @param size Number of input bytes.
     * @throws std::logic_error   after `finish()` has been called.
     * @throws std::runtime_error when the backend rejects the input.
     */
    void update(const void* data, std::size_t size);

    /**
     * @brief Feeds a byte span.
     *
     * @param data The input bytes.
     */
    void update(std::span<const std::byte> data);

    /**
     * @brief Feeds a MemoryStream-wrapped byte range.
     *
     * @param stream The wrapped input bytes.
     */
    void update(const MemoryStream& stream);

    /**
     * @brief Feeds a byte span through the stream operator.
     *
     * @param data The input bytes.
     * @return This hasher, for chaining.
     */
    Hasher& operator<<(std::span<const std::byte> data);

    /**
     * @brief Feeds a MemoryStream-wrapped byte range through the stream operator.
     *
     * @param stream The wrapped input bytes.
     * @return This hasher, for chaining.
     */
    Hasher& operator<<(const MemoryStream& stream);

    /**
     * @brief Feeds UTF-8 text through the stream operator.
     *
     * @param text The input text.
     * @return This hasher, for chaining.
     */
    Hasher& operator<<(std::u8string_view text);

    /**
     * @brief Returns the digest length in bytes.
     *
     * @return The length produced by `finish()`.
     */
    std::size_t size() const;

    /**
     * @brief Finalizes and returns the digest.
     *
     * @return The digest bytes, `size()` long; cached, so repeated calls answer the same bytes.
     * @throws std::runtime_error when the backend cannot finalize the digest.
     */
    std::vector<std::byte> finish();

    /**
     * @brief Returns the selected digest.
     *
     * @return The digest this hasher was created for.
     */
    Digest algorithm() const;

  private:
    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_CRYPTO_NS_END
