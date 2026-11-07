#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "crypto_global.hpp"

VN_CRYPTO_NS_BEGIN

/**
 * @brief AES-CBC with PKCS#7 padding, for formats that already use it.
 *
 * @details
 * - **This is not the default cipher.** CBC gives confidentiality only: it authenticates nothing, and a wrong key or a
 *   tampered ciphertext surfaces as garbage or as a padding error, not as "forged". Use `AesGcm` unless an existing
 *   format forces CBC (a container you must read, a peer you cannot change).
 * - The object streams: `update()` emits whole blocks and holds one partial block back, `finish()` pads, encrypts the
 *   last block and answers it. A decryptor keeps the final block until `finish()` so the padding can be stripped.
 * - `seal()` / `open()` are the one-shot forms; `open()` rejects a length that is not a whole number of blocks and a
 *   padding byte pattern that PKCS#7 does not allow.
 *
 * @warning
 * - Without a MAC, CBC is vulnerable to padding-oracle attacks: an attacker learns from the difference between "bad
 *   padding" and "good padding". Do not decrypt attacker-supplied ciphertext with this alone; an authenticated format
 *   (GCM) or an encrypt-then-MAC scheme is the requirement, not an optimisation.
 * - The IV must be unpredictable and must never repeat under one key. It is not secret, so it travels with the data.
 * - Not thread-safe: one object belongs to one producer.
 */
class VN_CRYPTO_API AesCbc {
  public:
    /** @brief Block size in bytes; also the IV size. */
    static constexpr std::size_t kBlockSize = 16;

    /**
     * @brief Creates an encrypting object.
     *
     * @param key AES key: 16, 24 or 32 bytes for AES-128/192/256.
     * @param iv  Initialization vector; exactly 16 bytes.
     * @return The encrypting object, ready for `update()`.
     * @throws std::invalid_argument when the key or IV size is not accepted.
     * @throws std::runtime_error    when the backend rejects the key or the IV.
     */
    static AesCbc encryptor(std::span<const std::byte> key, std::span<const std::byte> iv);

    /**
     * @brief Creates a decrypting object.
     *
     * @param key AES key: 16, 24 or 32 bytes.
     * @param iv  The initialization vector the data was encrypted with.
     * @return The decrypting object, ready for `update()`.
     * @throws std::invalid_argument when the key or IV size is not accepted.
     * @throws std::runtime_error    when the backend rejects the key or the IV.
     */
    static AesCbc decryptor(std::span<const std::byte> key, std::span<const std::byte> iv);

    ~AesCbc();

    /**
     * @brief Moves the cipher state; the moved-from object must not be used again.
     *
     * @param other The object to move from.
     */
    AesCbc(AesCbc&& other) noexcept;

    AesCbc& operator=(AesCbc&& other) noexcept;

    AesCbc(const AesCbc&)            = delete;
    AesCbc& operator=(const AesCbc&) = delete;

    /**
     * @brief Processes one chunk and answers the whole blocks it completed.
     *
     * @details
     * - Output length is a multiple of 16 and never longer than the input plus one held-back partial block; a chunk
     *   smaller than the held-back block yields nothing, which is normal.
     *
     * @param data The input bytes; any length.
     * @return The output bytes completed by this chunk (may be empty).
     * @throws std::logic_error   after `finish()` has been called.
     * @throws std::runtime_error when the backend rejects the chunk.
     */
    std::vector<std::byte> update(std::span<const std::byte> data);

    /**
     * @brief Finishes the operation and answers the last block.
     *
     * @details
     * - An encryptor pads with PKCS#7 (1..16 bytes) and answers one block, so the ciphertext always grows to the next
     *   block boundary.
     * - A decryptor answers the held-back final block minus its padding, so the plaintext is never longer than the
     *   ciphertext.
     *
     * @return The trailing output bytes; empty when a decryptor had nothing buffered.
     * @throws std::logic_error   when called twice.
     * @throws std::runtime_error when decryption fails, including a malformed PKCS#7 padding.
     */
    std::vector<std::byte> finish();

    /**
     * @brief Returns whether this object encrypts rather than decrypts.
     *
     * @return True for an encryptor.
     */
    bool isEncrypting() const;

    /**
     * @brief Encrypts a byte range in one call, padding it with PKCS#7.
     *
     * @param key       AES key: 16, 24 or 32 bytes.
     * @param iv        Initialization vector; exactly 16 bytes.
     * @param plaintext The bytes to encrypt; any length.
     * @return The ciphertext, a multiple of 16 bytes and at least one block longer than the plaintext.
     * @throws std::invalid_argument when a size is not accepted.
     * @throws std::runtime_error    when the backend rejects the key or the input.
     */
    static std::vector<std::byte> seal(std::span<const std::byte> key, std::span<const std::byte> iv,
                                       std::span<const std::byte> plaintext);

    /**
     * @brief Decrypts a byte range in one call and strips the PKCS#7 padding.
     *
     * @param key        AES key: 16, 24 or 32 bytes.
     * @param iv         The initialization vector the data was encrypted with.
     * @param ciphertext The encrypted bytes; must be a non-empty multiple of 16.
     * @return The plaintext.
     * @throws std::invalid_argument when the length is not a whole number of blocks.
     * @throws std::runtime_error    when decryption fails or the padding is malformed.
     */
    static std::vector<std::byte> open(std::span<const std::byte> key, std::span<const std::byte> iv,
                                       std::span<const std::byte> ciphertext);

  private:
    struct Impl;

    /**
     * @brief Adopts a state object built by one of the factories.
     *
     * @param state The state produced by `encryptor()` or `decryptor()`.
     */
    explicit AesCbc(std::unique_ptr<Impl> state);

    std::unique_ptr<Impl> impl;
};

VN_CRYPTO_NS_END
