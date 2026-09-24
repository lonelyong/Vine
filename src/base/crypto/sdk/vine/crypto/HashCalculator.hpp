#pragma once
#include "crypto_global.hpp"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include <vine/MemoryStream.hpp>

VN_CRYPTO_NS_BEGIN

/**
 * @brief Incremental hash calculator for a runtime-selected algorithm.
 *
 * Data is fed in chunks through write() or operator<< and the digest is
 * produced once at the end. Supports every algorithm exposed by Hash.
 */
class VN_CRYPTO_API HashCalculator
{
  public:
    /**
     * @brief Supported hash algorithms.
     */
    enum Algorithm
    {
        ALG_MD5,
        ALG_SHA1,
        ALG_SHA224,
        ALG_SHA256,
        ALG_SHA384,
        ALG_SHA512,
        ALG_SHA3_224,
        ALG_SHA3_256,
        ALG_SHA3_384,
        ALG_SHA3_512,
    };

    /**
     * @brief Constructs a calculator for the given algorithm.
     *
     * @param algorithm The hash algorithm.
     */
    explicit HashCalculator(Algorithm algorithm);

    /**
     * @brief Constructs a calculator from an algorithm name.
     *
     * Names are matched case-insensitively with either '-' or '_' separators,
     * e.g. "md5", "SHA-256" or "sha3_512".
     *
     * @param name The algorithm name.
     * @throws std::invalid_argument when the name is unknown.
     */
    explicit HashCalculator(std::u8string_view name);

    ~HashCalculator();

    HashCalculator(HashCalculator&&) noexcept;
    HashCalculator& operator=(HashCalculator&&) noexcept;

    HashCalculator(const HashCalculator&) = delete;
    HashCalculator& operator=(const HashCalculator&) = delete;

    /**
     * @brief Feeds a byte range into the hash.
     *
     * @param data Pointer to the input bytes.
     * @param size Number of input bytes.
     * @throws std::logic_error after digest() has been called.
     */
    void write(const void* data, std::size_t size);

    /**
     * @brief Feeds a MemoryStream-wrapped byte range into the hash.
     *
     * @param stream The wrapped input bytes.
     */
    void write(const MemoryStream& stream);

    /**
     * @brief Feeds a MemoryStream-wrapped byte range through the stream operator.
     *
     * @param stream The wrapped input bytes.
     * @return This calculator, for chaining.
     */
    HashCalculator& operator<<(const MemoryStream& stream);

    /**
     * @brief Feeds UTF-8 text through the stream operator.
     *
     * @param text The input text.
     * @return This calculator, for chaining.
     */
    HashCalculator& operator<<(std::u8string_view text);

    /**
     * @brief Returns the digest size of the selected algorithm in bytes.
     *
     * @return The digest size.
     */
    std::size_t digestSize() const;

    /**
     * @brief Finalizes and returns the digest.
     *
     * The result is cached, so repeated calls return the same bytes.
     *
     * @return The digest (length equals digestSize()).
     */
    std::vector<unsigned char> digest();

    /**
     * @brief Returns the selected algorithm.
     *
     * @return The algorithm.
     */
    Algorithm algorithm() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

VN_CRYPTO_NS_END
