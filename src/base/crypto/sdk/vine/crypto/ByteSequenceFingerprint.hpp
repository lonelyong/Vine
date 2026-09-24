#pragma once
#include "crypto_global.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <span>

VN_CRYPTO_NS_BEGIN

/**
 * @brief Byte-sequence fingerprint for efficiently comparing two byte ranges.
 *
 * Fixed-size storage (128-byte fingerprint + 8-byte size + 1-byte mode = 137
 * bytes) with zero dynamic allocation, suitable for bulk storage in
 * std::vector.
 *
 * The mode is derived from the data size; every constructor behaves the same:
 *  - size <= 128 B          : RAW — stores the raw bytes.
 *  - 128 B < size <= 1 MiB  : SHA256 — stores the full SHA-256.
 *  - size > 1 MiB           : SAMPLED_SHA256 — adaptive sampled SHA-256.
 *
 * operator== compares size_, mode_ and fingerprint_ together. Fingerprints
 * with different modes never compare equal, even when the stored bytes
 * happen to match. Two SAMPLED_SHA256 fingerprints being equal only means
 * their sampled signatures match; full content equality is not guaranteed.
 *
 * @note The input stream must support seek; a non-seekable stream throws
 *       std::runtime_error.
 */
class VN_CRYPTO_API ByteSequenceFingerprint
{
    // 类型声明区块
  public:
    /** @brief Fixed size of the fingerprint storage array (bytes). Also the
     *         threshold below which small data is stored raw. */
    static constexpr std::size_t kFingerprintSize = 128;

    /** @brief Fingerprint generation mode. */
    enum FingerprintMode : std::uint8_t
    {
        RAW,
        SHA256,
        SAMPLED_SHA256,
    };

    friend struct std::hash<ByteSequenceFingerprint>;

    // 构造函数区块
  public:
    /** @brief Constructs an empty fingerprint. */
    ByteSequenceFingerprint();

    /**
     * @brief Constructs a fingerprint from a file path.
     *
     * @param path Path to the file.
     * @throws std::runtime_error when the file cannot be opened or read.
     */
    explicit ByteSequenceFingerprint(const std::filesystem::path& path);

    /**
     * @brief Constructs a fingerprint from an input stream.
     *
     * The stream must support seek, which is used to obtain the size and the
     * sample positions. The stream position is restored afterwards.
     *
     * @param stream The input stream.
     * @throws std::runtime_error when the stream is not seekable or reading
     *         fails.
     */
    explicit ByteSequenceFingerprint(std::istream& stream);

    /**
     * @brief Constructs a fingerprint from an in-memory byte range.
     *
     * @param data The input byte range.
     */
    explicit ByteSequenceFingerprint(std::span<const std::byte> data);

    // 方法区块
  public:
    /**
     * @brief Compares two fingerprints for equality (size + mode + bytes).
     *
     * @param rhs The fingerprint to compare with.
     * @return true if the fingerprints are equal.
     */
    bool operator==(const ByteSequenceFingerprint& rhs) const noexcept;

    /**
     * @brief Compares two fingerprints for inequality.
     *
     * @param rhs The fingerprint to compare with.
     * @return true if the fingerprints differ.
     */
    bool operator!=(const ByteSequenceFingerprint& rhs) const noexcept;

    /**
     * @brief Compares two fingerprints for ordering.
     *
     * Lexicographic over (mode, size, bytes); consistent with operator==, so
     * fingerprints equal under == never order one below the other. Enables
     * use as a std::map key (e.g. vn::runtime::InMemoryCache).
     *
     * @param rhs The fingerprint to compare with.
     * @return true when this fingerprint precedes rhs.
     */
    bool operator<(const ByteSequenceFingerprint& rhs) const noexcept
    {
        if (mode_ != rhs.mode_)
        {
            return mode_ < rhs.mode_;
        }
        if (size_ != rhs.size_)
        {
            return size_ < rhs.size_;
        }
        return std::memcmp(fingerprint_.data(), rhs.fingerprint_.data(), kFingerprintSize) < 0;
    }

    /**
     * @brief Returns the fingerprint generation mode.
     *
     * @return The generation mode.
     */
    FingerprintMode mode() const noexcept
    {
        return mode_;
    }

    /**
     * @brief Returns the size of the original data in bytes.
     *
     * @return The original data size.
     */
    std::uint64_t size() const noexcept
    {
        return size_;
    }

    /**
     * @brief Checks whether the fingerprint wraps non-empty data.
     *
     * @return true if the original data size is greater than zero.
     */
    explicit operator bool() const noexcept
    {
        return size_ > 0;
    }

    // 字段区块
  private:
    std::uint64_t size_{ 0 };
    FingerprintMode mode_{ FingerprintMode::RAW };
    std::array<std::byte, kFingerprintSize> fingerprint_{};
};

VN_CRYPTO_NS_END

template <>
struct std::hash<VN_ROOT_NS::crypto::ByteSequenceFingerprint>
{
    /**
     * @brief Computes a hash of the fingerprint.
     *
     * Only the meaningful bytes are hashed: all 128 bytes for RAW mode, the
     * 32 digest bytes otherwise.
     *
     * @param fp The fingerprint.
     * @return The hash value.
     */
    std::size_t operator()(const VN_ROOT_NS::crypto::ByteSequenceFingerprint& fp) const noexcept
    {
        std::size_t h = fp.size_;

        auto combine = [&h](std::size_t v) noexcept
        {
            h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        };

        combine(static_cast<std::size_t>(fp.mode_));

        const std::size_t bytes_to_hash = (fp.mode_ == VN_ROOT_NS::crypto::ByteSequenceFingerprint::FingerprintMode::RAW)
                                              ? fp.fingerprint_.size()
                                              : 32;

        for (std::size_t i = 0; i < bytes_to_hash; ++i) {
            combine(std::to_integer<std::uint8_t>(fp.fingerprint_[i]));
        }

        return h;
    }
};
