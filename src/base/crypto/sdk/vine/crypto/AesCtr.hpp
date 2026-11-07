#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "crypto_global.hpp"

VN_CRYPTO_NS_BEGIN

/**
 * @brief AES-CTR: a keystream cipher, for formats that already use it.
 *
 * @details
 * - **This is not the default cipher.** CTR gives confidentiality only and is unforgiving: reusing a counter block
 *   under one key leaks the XOR of two plaintexts, and a flipped ciphertext bit flips the same plaintext bit. Use
 *   `AesGcm` unless an existing format forces CTR.
 * - The counter block is the IV: encryption and decryption are the same operation, so there is one factory (`create()`
 *   / `apply()`) rather than a pair.
 * - The cipher is a stream: output length always equals input length, no padding is involved, and chunks may be any
 *   size.
 *
 * @warning
 * - The caller owns the counter. It must never repeat under one key, which in practice means a per-message nonce plus
 *   a counter that cannot wrap; a random 16-byte block is safe only up to the birthday bound.
 * - There is no authentication. Anyone can flip bits and the result stays "valid", so any untrusted input needs a MAC
 *   over the ciphertext (encrypt-then-MAC) — or `AesGcm`, which does both.
 * - Not thread-safe: one object belongs to one producer.
 */
class VN_CRYPTO_API AesCtr {
  public:
    /** @brief Counter block size in bytes. */
    static constexpr std::size_t kBlockSize = 16;

    /**
     * @brief Creates a cipher object for the given key and initial counter block.
     *
     * @param key          AES key: 16, 24 or 32 bytes for AES-128/192/256.
     * @param counterBlock Initial counter block; exactly 16 bytes.
     * @return The cipher object, ready for `update()`.
     * @throws std::invalid_argument when the key or counter size is not accepted.
     * @throws std::runtime_error    when the backend rejects the key or the counter.
     */
    static AesCtr create(std::span<const std::byte> key, std::span<const std::byte> counterBlock);

    ~AesCtr();

    /**
     * @brief Moves the cipher state; the moved-from object must not be used again.
     *
     * @param other The object to move from.
     */
    AesCtr(AesCtr&& other) noexcept;

    AesCtr& operator=(AesCtr&& other) noexcept;

    AesCtr(const AesCtr&)            = delete;
    AesCtr& operator=(const AesCtr&) = delete;

    /**
     * @brief Transforms one chunk; encryption and decryption are the same call.
     *
     * @param data The input bytes; any length.
     * @return The output bytes, exactly as long as the input.
     * @throws std::logic_error   after `finish()` has been called.
     * @throws std::runtime_error when the backend rejects the chunk.
     */
    std::vector<std::byte> update(std::span<const std::byte> data);

    /**
     * @brief Ends the stream.
     *
     * @details
     * - A keystream has no trailing block, so this answers nothing; it exists so the object follows the same
     *   `update()` / `finish()` shape as the other incremental types, and so misuse after it is caught.
     *
     * @return An empty buffer.
     * @throws std::logic_error when called twice.
     */
    std::vector<std::byte> finish();

    /**
     * @brief Transforms a byte range in one call.
     *
     * @param key          AES key: 16, 24 or 32 bytes.
     * @param counterBlock Initial counter block; exactly 16 bytes.
     * @param data         The bytes to transform.
     * @return The transformed bytes, the same length as the input.
     * @throws std::invalid_argument when a size is not accepted.
     * @throws std::runtime_error    when the backend rejects the key or the input.
     */
    static std::vector<std::byte> apply(std::span<const std::byte> key, std::span<const std::byte> counterBlock,
                                        std::span<const std::byte> data);

  private:
    struct Impl;

    /**
     * @brief Adopts a state object built by the factory.
     *
     * @param state The state produced by `create()`.
     */
    explicit AesCtr(std::unique_ptr<Impl> state);

    std::unique_ptr<Impl> impl;
};

VN_CRYPTO_NS_END
