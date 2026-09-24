#pragma once
#include "crypto_global.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>

VN_CRYPTO_NS_BEGIN

/**
 * @brief Provides cryptographic hash functions.
 */
class VN_CRYPTO_API Hash {

  public:
    /**
     * @brief Computes the SHA-256 digest of a byte range.
     *
     * @param data Pointer to the input bytes.
     * @param size Number of input bytes.
     * @return The 32-byte digest.
     */
    static std::array<std::uint8_t, 32> sha256(const void* data, std::size_t size);

    /**
     * @brief Computes the MD5 digest of a byte range.
     *
     * @param data Pointer to the input bytes.
     * @param size Number of input bytes.
     * @return The 16-byte digest.
     */
    static std::array<std::uint8_t, 16> md5(const void* data, std::size_t size);

    /**
     * @brief Computes the SHA-1 digest of a byte range.
     *
     * @param data Pointer to the input bytes.
     * @param size Number of input bytes.
     * @return The 20-byte digest.
     */
    static std::array<std::uint8_t, 20> sha1(const void* data, std::size_t size);

    /**
     * @brief Computes the SHA-224 digest of a byte range.
     *
     * @param data Pointer to the input bytes.
     * @param size Number of input bytes.
     * @return The 28-byte digest.
     */
    static std::array<std::uint8_t, 28> sha224(const void* data, std::size_t size);

    /**
     * @brief Computes the SHA-384 digest of a byte range.
     *
     * @param data Pointer to the input bytes.
     * @param size Number of input bytes.
     * @return The 48-byte digest.
     */
    static std::array<std::uint8_t, 48> sha384(const void* data, std::size_t size);

    /**
     * @brief Computes the SHA-512 digest of a byte range.
     *
     * @param data Pointer to the input bytes.
     * @param size Number of input bytes.
     * @return The 64-byte digest.
     */
    static std::array<std::uint8_t, 64> sha512(const void* data, std::size_t size);

    /**
     * @brief Computes the SHA3-224 digest of a byte range.
     *
     * @param data Pointer to the input bytes.
     * @param size Number of input bytes.
     * @return The 28-byte digest.
     */
    static std::array<std::uint8_t, 28> sha3_224(const void* data, std::size_t size);

    /**
     * @brief Computes the SHA3-256 digest of a byte range.
     *
     * @param data Pointer to the input bytes.
     * @param size Number of input bytes.
     * @return The 32-byte digest.
     */
    static std::array<std::uint8_t, 32> sha3_256(const void* data, std::size_t size);

    /**
     * @brief Computes the SHA3-384 digest of a byte range.
     *
     * @param data Pointer to the input bytes.
     * @param size Number of input bytes.
     * @return The 48-byte digest.
     */
    static std::array<std::uint8_t, 48> sha3_384(const void* data, std::size_t size);

    /**
     * @brief Computes the SHA3-512 digest of a byte range.
     *
     * @param data Pointer to the input bytes.
     * @param size Number of input bytes.
     * @return The 64-byte digest.
     */
    static std::array<std::uint8_t, 64> sha3_512(const void* data, std::size_t size);

    /**
     * @brief Computes the SHA-256 digest of a stream.
     *
     * @param in The input stream.
     * @return The 32-byte digest.
     */
    static std::array<std::uint8_t, 32> sha256(std::ostream& in);

    /**
     * @brief Computes the MD5 digest of a stream.
     *
     * @param in The input stream.
     * @return The 16-byte digest.
     */
    static std::array<std::uint8_t, 16> md5(std::ostream& in);

    /**
     * @brief Computes the SHA-1 digest of a stream.
     *
     * @param in The input stream.
     * @return The 20-byte digest.
     */
    static std::array<std::uint8_t, 20> sha1(std::ostream& in);

    /**
     * @brief Computes the SHA-224 digest of a stream.
     *
     * @param in The input stream.
     * @return The 28-byte digest.
     */
    static std::array<std::uint8_t, 28> sha224(std::ostream& in);

    /**
     * @brief Computes the SHA-384 digest of a stream.
     *
     * @param in The input stream.
     * @return The 48-byte digest.
     */
    static std::array<std::uint8_t, 48> sha384(std::ostream& in);

    /**
     * @brief Computes the SHA-512 digest of a stream.
     *
     * @param in The input stream.
     * @return The 64-byte digest.
     */
    static std::array<std::uint8_t, 64> sha512(std::ostream& in);

    /**
     * @brief Computes the SHA3-224 digest of a stream.
     *
     * @param in The input stream.
     * @return The 28-byte digest.
     */
    static std::array<std::uint8_t, 28> sha3_224(std::ostream& in);

    /**
     * @brief Computes the SHA3-256 digest of a stream.
     *
     * @param in The input stream.
     * @return The 32-byte digest.
     */
    static std::array<std::uint8_t, 32> sha3_256(std::ostream& in);

    /**
     * @brief Computes the SHA3-384 digest of a stream.
     *
     * @param in The input stream.
     * @return The 48-byte digest.
     */
    static std::array<std::uint8_t, 48> sha3_384(std::ostream& in);

    /**
     * @brief Computes the SHA3-512 digest of a stream.
     *
     * @param in The input stream.
     * @return The 64-byte digest.
     */
    static std::array<std::uint8_t, 64> sha3_512(std::ostream& in);
};

VN_CRYPTO_NS_END
