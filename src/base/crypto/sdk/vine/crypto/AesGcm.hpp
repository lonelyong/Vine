#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "crypto_global.hpp"

VN_CRYPTO_NS_BEGIN

/**
 * @brief AES-GCM authenticated encryption (NIST SP 800-38D).
 *
 * @details
 * - One object is one direction: `encryptor()` produces ciphertext and a tag, `decryptor()` verifies a tag and
 *   produces plaintext. There is no mode flag to get wrong and no way to call the wrong direction by accident.
 * - The ciphertext is the same length as the plaintext; the 16-byte tag travels beside it, not inside it.
 * - AAD (associated data) is authenticated but not encrypted: headers, sequence numbers, file names. It is supplied
 *   once, at construction, because GCM requires every associated byte before the first ciphertext byte.
 * - The object streams: `update()` emits output for the bytes it was given and holds no input, so a file of any size
 *   goes through with a buffer per chunk. `seal()` / `open()` are the one-shot forms built on the same path.
 *
 * @warning
 * - **A decryptor's `update()` output is not authentic until `finish()` returns.** The tag can only be checked once
 *   the last byte arrived, so a failed `finish()` means every byte the object emitted must be discarded. Writing
 *   decrypted chunks to their final destination before `finish()` succeeds is only safe when the destination can be
 *   rolled back.
 * - The IV must never repeat under one key. With a random IV, 12 bytes and the birthday bound apply; a counter is only
 *   safe if the caller can guarantee it does not wrap.
 * - A failed `finish()` leaves the object unusable; create a new one to retry with another key or tag.
 */
class VN_CRYPTO_API AesGcm {
  public:
    /** @brief Tag size in bytes; a shorter tag has no advantage here. */
    static constexpr std::size_t kTagSize = 16;

    /** @brief A ciphertext together with the tag that authenticates it. */
    struct Sealed
    {
        /// Encrypted bytes; same length as the plaintext.
        std::vector<std::byte> ciphertext;

        /// Authentication tag over the ciphertext and the associated data.
        std::array<std::byte, kTagSize> tag{};
    };

    /**
     * @brief Creates an encrypting object.
     *
     * @param key AES key: 16, 24 or 32 bytes for AES-128/192/256.
     * @param iv  Initialization vector; 12 bytes is the recommended size.
     * @param aad Associated data to authenticate; may be empty.
     * @return The encrypting object, ready for `update()`.
     * @throws std::invalid_argument when the key or IV size is not accepted.
     * @throws std::runtime_error    when the backend rejects the key or the IV.
     */
    static AesGcm encryptor(std::span<const std::byte> key, std::span<const std::byte> iv,
                            std::span<const std::byte> aad = {});

    /**
     * @brief Creates a decrypting object.
     *
     * @param key AES key: 16, 24 or 32 bytes.
     * @param iv  Initialization vector that was used for encryption.
     * @param tag The tag to verify at `finish()`.
     * @param aad Associated data that was authenticated; must match the encryption side.
     * @return The decrypting object, ready for `update()`.
     * @throws std::invalid_argument when the key, IV or tag size is not accepted.
     * @throws std::runtime_error    when the backend rejects the key or the IV.
     */
    static AesGcm decryptor(std::span<const std::byte> key, std::span<const std::byte> iv,
                            std::span<const std::byte> tag, std::span<const std::byte> aad = {});

    ~AesGcm();

    /**
     * @brief Moves the cipher state; the moved-from object must not be used again.
     *
     * @param other The object to move from.
     */
    AesGcm(AesGcm&& other) noexcept;

    AesGcm& operator=(AesGcm&& other) noexcept;

    AesGcm(const AesGcm&)            = delete;
    AesGcm& operator=(const AesGcm&) = delete;

    /**
     * @brief Processes one chunk and answers its output bytes.
     *
     * @details
     * - The output is exactly as long as the input: GCM turns plaintext into ciphertext one byte for one.
     * - Chunks may be any size, including zero bytes; the result does not depend on how the input was split.
     *
     * @param data The input bytes.
     * @return The output bytes for this chunk: ciphertext on an encryptor, plaintext on a decryptor.
     * @throws std::logic_error   after `finish()` has been called.
     * @throws std::runtime_error when the backend rejects the chunk.
     */
    std::vector<std::byte> update(std::span<const std::byte> data);

    /**
     * @brief Processes one chunk and appends its output to `out`.
     *
     * @details
     * - This overload exists for the caller that owns a reusable buffer and does not want one allocation per chunk.
     *
     * @param data The input bytes.
     * @param out  Buffer that receives the output; resized to `data.size()`.
     * @throws std::logic_error   after `finish()` has been called.
     * @throws std::runtime_error when the backend rejects the chunk.
     */
    void update(std::span<const std::byte> data, std::vector<std::byte>& out);

    /**
     * @brief Finishes the operation and answers any remaining output.
     *
     * @details
     * - GCM has no trailing block, so the returned bytes are always empty; they exist so the signature matches the
     *   other incremental objects and so a future mode can flush its tail.
     * - On an encryptor this writes the tag, which `tag()` then answers.
     * - On a decryptor this is where the tag is checked: success means every byte returned by `update()` is
     *   authentic, failure means none of it is.
     *
     * @return The remaining output bytes (empty for GCM).
     * @throws std::logic_error   when called twice.
     * @throws std::runtime_error when decryption rejects the tag or the ciphertext.
     */
    std::vector<std::byte> finish();

    /**
     * @brief Returns whether this object encrypts rather than decrypts.
     *
     * @return True for an encryptor.
     */
    bool isEncrypting() const;

    /**
     * @brief Returns the tag produced by an encryptor.
     *
     * @return The 16-byte tag; only valid after `finish()`.
     * @throws std::logic_error before `finish()`, or on a decrypting object.
     */
    std::array<std::byte, kTagSize> tag() const;

    /**
     * @brief Seals a byte range in one call.
     *
     * @param key       AES key: 16, 24 or 32 bytes.
     * @param iv        Initialization vector; 12 bytes is recommended.
     * @param plaintext The bytes to encrypt.
     * @param aad       Associated data to authenticate; may be empty.
     * @return The ciphertext and its tag.
     * @throws std::invalid_argument when a size is not accepted.
     * @throws std::runtime_error    when the backend rejects the key or the input.
     */
    static Sealed seal(std::span<const std::byte> key, std::span<const std::byte> iv,
                       std::span<const std::byte> plaintext, std::span<const std::byte> aad = {});

    /**
     * @brief Opens a sealed byte range in one call.
     *
     * @param key        AES key: 16, 24 or 32 bytes.
     * @param iv         The IV that was used for encryption.
     * @param ciphertext The encrypted bytes.
     * @param tag        The tag to verify.
     * @param aad        Associated data that was authenticated; may be empty.
     * @return The plaintext.
     * @throws std::runtime_error when the tag does not verify.
     */
    static std::vector<std::byte> open(std::span<const std::byte> key, std::span<const std::byte> iv,
                                       std::span<const std::byte> ciphertext, std::span<const std::byte> tag,
                                       std::span<const std::byte> aad = {});

  private:
    struct Impl;

    /**
     * @brief Adopts a state object built by one of the factories.
     *
     * @param state The state produced by `encryptor()` or `decryptor()`.
     */
    explicit AesGcm(std::unique_ptr<Impl> state);

    std::unique_ptr<Impl> impl;
};

VN_CRYPTO_NS_END
