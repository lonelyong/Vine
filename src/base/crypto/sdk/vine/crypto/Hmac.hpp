#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "crypto_global.hpp"
#include "Digest.hpp"

VN_CRYPTO_NS_BEGIN

/**
 * @brief Computes an HMAC (RFC 2104) over a byte range in one call.
 *
 * @details
 * - The tag length equals `digestSize(algorithm)`; a key of any length is accepted, because the backend hashes keys
 *   longer than the block size, as RFC 2104 requires.
 * - Use this for data already in memory; the `Hmac` class is the incremental form for data that arrives in chunks.
 *
 * @param algorithm The digest to use inside the construction.
 * @param key       The shared secret.
 * @param data      The bytes to authenticate.
 * @return The tag, `digestSize(algorithm)` bytes long.
 * @throws std::runtime_error when the backend rejects the key or the input.
 */
std::vector<std::byte> VN_CRYPTO_API hmac(Digest algorithm, std::span<const std::byte> key,
                                          std::span<const std::byte> data);

/**
 * @brief Compares a computed tag with the one that was received.
 *
 * @details
 * - Every byte is compared, so the position of the first difference does not leak through timing.
 *
 * @param computed The tag this side calculated.
 * @param received The tag that arrived.
 * @return True when the lengths match and every byte is equal.
 */
bool VN_CRYPTO_API macEquals(std::span<const std::byte> computed, std::span<const std::byte> received);

/**
 * @brief Incremental HMAC over a digest chosen at run time.
 *
 * @details
 * - Construct it with the digest and the key, feed the message with `update()`, read the tag once with `finish()`.
 * - The tag length equals `size()`, which is the digest length.
 * - The object is move-only: copying would duplicate the keyed backend state silently.
 *
 * @note
 * - libsodium and RustCrypto split the same way: a one-shot function (`hmac()`) and an object holding the running
 *   state. The key stays inside the backend context until the object is destroyed.
 *
 * @warning
 * - Not thread-safe: one instance belongs to one producer.
 */
class VN_CRYPTO_API Hmac {
  public:
    /**
     * @brief Starts a tag computation.
     *
     * @param algorithm The digest to use inside the construction.
     * @param key       The shared secret.
     * @throws std::runtime_error when the backend rejects the context or the key.
     */
    Hmac(Digest algorithm, std::span<const std::byte> key);

    /**
     * @brief Starts a tag computation, selecting the digest by name.
     *
     * @param digestName Digest name, as accepted by `digestFromName`.
     * @param key        The shared secret.
     * @throws std::invalid_argument when the name is unknown.
     * @throws std::runtime_error    when the backend rejects the context or the key.
     */
    Hmac(std::u8string_view digestName, std::span<const std::byte> key);

    ~Hmac();

    /**
     * @brief Moves the keyed state; the moved-from object must not be used again.
     *
     * @param other The object to move from.
     */
    Hmac(Hmac&& other) noexcept;

    Hmac& operator=(Hmac&& other) noexcept;

    Hmac(const Hmac&)            = delete;
    Hmac& operator=(const Hmac&) = delete;

    /**
     * @brief Feeds a byte range into the computation.
     *
     * @param data Pointer to the input bytes; may be null when `size` is zero.
     * @param size Number of input bytes.
     * @throws std::logic_error   after `finish()` has been called.
     * @throws std::runtime_error when the backend rejects the input.
     */
    void update(const void* data, std::size_t size);

    /**
     * @brief Feeds a byte span into the computation.
     *
     * @param data The input bytes.
     */
    void update(std::span<const std::byte> data);

    /**
     * @brief Feeds a byte span through the stream operator.
     *
     * @param data The input bytes.
     * @return This object, for chaining.
     */
    Hmac& operator<<(std::span<const std::byte> data);

    /**
     * @brief Feeds UTF-8 text through the stream operator.
     *
     * @param text The input text.
     * @return This object, for chaining.
     */
    Hmac& operator<<(std::u8string_view text);

    /**
     * @brief Returns the tag length in bytes.
     *
     * @return The length produced by `finish()`.
     */
    std::size_t size() const;

    /**
     * @brief Finalizes and returns the tag.
     *
     * @return The tag, `size()` bytes long; cached, so repeated calls answer the same bytes.
     * @throws std::runtime_error when the backend cannot finalize the tag.
     */
    std::vector<std::byte> finish();

    /**
     * @brief Returns the selected digest.
     *
     * @return The digest this computation was created for.
     */
    Digest algorithm() const;

  private:
    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_CRYPTO_NS_END
