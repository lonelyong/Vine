#include <vine/crypto/ByteSequenceFingerprint.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <istream>
#include <span>
#include <stdexcept>
#include <vector>

#include <vine/crypto/Hash.hpp>
#include <vine/crypto/HashCalculator.hpp>

VN_CRYPTO_NS_BEGIN

namespace
{

using FpMode = ByteSequenceFingerprint::FingerprintMode;

/** @brief Small-data threshold shared by every constructor (128 B). */
constexpr std::size_t kSmallThreshold = ByteSequenceFingerprint::kFingerprintSize;

/** @brief Size above which the sampled mode is used (1 MiB). */
constexpr std::uint64_t kMediumThreshold = 1024ULL * 1024ULL;

/** @brief Hard cap on the total sampled bytes (1 MiB). */
constexpr std::size_t kMaxTotalSampleBytes = 1024 * 1024;

/**
 * @brief Adaptive sampling strategy selected from the data size.
 *
 * | Data size        | Regions | Bytes/region | Total sampled |
 * |------------------|---------|--------------|---------------|
 * | <= 16 MiB        | 5       | 4 KiB        | ~20 KiB       |
 * | <= 256 MiB       | 7       | 8 KiB        | ~56 KiB       |
 * | <= 4 GiB         | 9       | 32 KiB       | ~288 KiB      |
 * | > 4 GiB (up to TB)| 11     | 64 KiB       | ~704 KiB      |
 *
 * The total sampled amount never exceeds kMaxTotalSampleBytes.
 */
struct SamplingConfig
{
    std::size_t num_regions;
    std::size_t bytes_per_region;
};

constexpr SamplingConfig chooseSamplingConfig(std::uint64_t data_size) noexcept
{
    if (data_size <= 16ULL * 1024ULL * 1024ULL) {
        return { 5, 4096 };
    }
    if (data_size <= 256ULL * 1024ULL * 1024ULL) {
        return { 7, 8192 };
    }
    if (data_size <= 4ULL * 1024ULL * 1024ULL * 1024ULL) {
        return { 9, 32768 };
    }
    return { 11, 65536 };
}

/**
 * @brief Writes a SHA-256 digest into the leading bytes of a fingerprint and
 *        zero-fills the remaining bytes.
 *
 * @param digest The 32-byte digest.
 * @param out The fingerprint array to fill.
 */
void writeDigestToFingerprint(const std::array<std::uint8_t, 32>& digest,
                              std::array<std::byte, ByteSequenceFingerprint::kFingerprintSize>& out) noexcept
{
    std::memcpy(out.data(), digest.data(), digest.size());
    std::fill(out.begin() + digest.size(), out.end(), std::byte{ 0 });
}

/**
 * @brief Generates the sample offsets for the given data size and config.
 *
 * The first region starts at the beginning, the last region ends at the end
 * of the data, and the middle regions are spread evenly in between.
 *
 * @param data_size The data size in bytes.
 * @param cfg The sampling config.
 * @return The list of sample offsets.
 */
std::vector<std::uint64_t> generateSampleOffsets(std::uint64_t data_size, SamplingConfig cfg)
{
    std::vector<std::uint64_t> offsets;
    offsets.reserve(cfg.num_regions);

    for (std::size_t r = 0; r < cfg.num_regions; ++r) {
        std::uint64_t offset;
        if (r == 0) {
            offset = 0;
        } else if (r == cfg.num_regions - 1) {
            offset = (data_size > cfg.bytes_per_region) ? (data_size - cfg.bytes_per_region) : 0;
        } else {
            offset = data_size * r / (cfg.num_regions - 1);
            if (offset < cfg.bytes_per_region) {
                offset = cfg.bytes_per_region;
            }
            if (offset + cfg.bytes_per_region > data_size) {
                offset = (data_size > cfg.bytes_per_region) ? (data_size - cfg.bytes_per_region) : 0;
            }
        }

        if (offset >= data_size) {
            continue;
        }
        offsets.push_back(offset);
    }

    return offsets;
}

/**
 * @brief Computes the sampled fingerprint of an in-memory byte range.
 *
 * @param data The input byte range (must exceed kMediumThreshold).
 * @param out The fingerprint array to fill.
 */
void computeSampledFromSpan(std::span<const std::byte> data,
                            std::array<std::byte, ByteSequenceFingerprint::kFingerprintSize>& out)
{
    const std::uint64_t           data_size = data.size();
    const SamplingConfig          cfg       = chooseSamplingConfig(data_size);
    const std::vector<std::uint64_t> offsets = generateSampleOffsets(data_size, cfg);

    std::vector<std::byte> sampled;
    sampled.reserve(cfg.num_regions * cfg.bytes_per_region);

    for (const std::uint64_t offset : offsets) {
        std::size_t to_read = cfg.bytes_per_region;
        if (offset + to_read > data_size) {
            to_read = static_cast<std::size_t>(data_size - offset);
        }

        if (sampled.size() + to_read > kMaxTotalSampleBytes) {
            to_read = kMaxTotalSampleBytes - sampled.size();
            if (to_read == 0) {
                break;
            }
        }

        sampled.insert(sampled.end(), data.data() + offset, data.data() + offset + to_read);
    }

    writeDigestToFingerprint(Hash::sha256(sampled.data(), sampled.size()), out);
}

/**
 * @brief Computes the sampled fingerprint of a seekable stream.
 *
 * @param stream The stream to read from.
 * @param data_size The data size in bytes.
 * @param out The fingerprint array to fill.
 * @throws std::runtime_error when a seek fails.
 */
void computeSampledFromStream(std::istream& stream,
                              std::uint64_t data_size,
                              std::array<std::byte, ByteSequenceFingerprint::kFingerprintSize>& out)
{
    const SamplingConfig          cfg       = chooseSamplingConfig(data_size);
    const std::vector<std::uint64_t> offsets = generateSampleOffsets(data_size, cfg);

    std::vector<std::byte> sampled;
    sampled.reserve(cfg.num_regions * cfg.bytes_per_region);
    std::vector<std::byte> read_buf(cfg.bytes_per_region);

    for (const std::uint64_t offset : offsets) {
        std::size_t to_read = cfg.bytes_per_region;
        if (offset + to_read > data_size) {
            to_read = static_cast<std::size_t>(data_size - offset);
        }

        if (sampled.size() + to_read > kMaxTotalSampleBytes) {
            to_read = kMaxTotalSampleBytes - sampled.size();
            if (to_read == 0) {
                break;
            }
        }

        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream) {
            throw std::runtime_error("ByteSequenceFingerprint: seek failed in sampled read");
        }

        stream.read(reinterpret_cast<char*>(read_buf.data()), static_cast<std::streamsize>(to_read));
        const std::size_t n = static_cast<std::size_t>(stream.gcount());
        if (n > 0) {
            sampled.insert(sampled.end(), read_buf.begin(), read_buf.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }

    writeDigestToFingerprint(Hash::sha256(sampled.data(), sampled.size()), out);
}

/**
 * @brief Computes the SHA-256 of a stream by streaming its bytes.
 *
 * @param stream The stream positioned at the start.
 * @param data_size Number of bytes to hash.
 * @return The 32-byte digest.
 */
std::array<std::uint8_t, 32> computeSha256FromStream(std::istream& stream, std::uint64_t data_size)
{
    HashCalculator        calc(HashCalculator::Algorithm::ALG_SHA256);
    std::array<char, 4096> buffer{};
    std::uint64_t         remaining = data_size;

    while (remaining > 0) {
        const auto to_read = static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), remaining));
        stream.read(buffer.data(), to_read);
        const std::streamsize n = stream.gcount();
        if (n <= 0) {
            break;
        }
        calc.write(buffer.data(), static_cast<std::size_t>(n));
        remaining -= static_cast<std::uint64_t>(n);
    }

    const std::vector<unsigned char> digest = calc.digest();
    std::array<std::uint8_t, 32>     out{};
    std::copy_n(digest.begin(), std::min<std::size_t>(digest.size(), out.size()), out.begin());
    return out;
}

/**
 * @brief Computes the fingerprint of a seekable stream.
 *
 * @param stream The stream to read from.
 * @param data_size The data size in bytes.
 * @param fp The fingerprint array to fill.
 * @param mode The fingerprint mode to fill.
 * @throws std::runtime_error when a read fails.
 */
void computeFromStream(std::istream& stream,
                       std::uint64_t data_size,
                       std::array<std::byte, ByteSequenceFingerprint::kFingerprintSize>& fp,
                       FpMode& mode)
{
    if (data_size == 0) {
        fp.fill(std::byte{ 0 });
        mode = FpMode::RAW;
        return;
    }

    if (data_size <= kSmallThreshold) {
        fp.fill(std::byte{ 0 });
        stream.clear();
        stream.seekg(0, std::ios::beg);
        stream.read(reinterpret_cast<char*>(fp.data()), static_cast<std::streamsize>(data_size));
        if (stream.bad()) {
            throw std::runtime_error("ByteSequenceFingerprint: stream read error in RAW mode");
        }
        mode = FpMode::RAW;
        return;
    }

    if (data_size <= kMediumThreshold) {
        stream.clear();
        stream.seekg(0, std::ios::beg);
        writeDigestToFingerprint(computeSha256FromStream(stream, data_size), fp);
        mode = FpMode::SHA256;
        return;
    }

    computeSampledFromStream(stream, data_size, fp);
    mode = FpMode::SAMPLED_SHA256;
}

} // namespace

ByteSequenceFingerprint::ByteSequenceFingerprint()
{
}

ByteSequenceFingerprint::ByteSequenceFingerprint(const std::filesystem::path& path)
{
    std::error_code      ec;
    const std::uint64_t file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw std::runtime_error("ByteSequenceFingerprint: cannot get file size for: " + path.string());
    }

    size_ = file_size;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("ByteSequenceFingerprint: cannot open file: " + path.string());
    }

    computeFromStream(file, size_, fingerprint_, mode_);
}

ByteSequenceFingerprint::ByteSequenceFingerprint(std::istream& stream)
{
    const std::streampos orig_pos = stream.tellg();

    stream.clear();
    stream.seekg(0, std::ios::end);
    const std::streampos end_pos = stream.tellg();

    if (end_pos >= 0 && stream.good()) {
        const std::uint64_t stream_size = static_cast<std::uint64_t>(end_pos);
        size_                           = stream_size;

        stream.seekg(0, std::ios::beg);
        computeFromStream(stream, size_, fingerprint_, mode_);

        stream.clear();
        if (orig_pos >= 0) {
            stream.seekg(orig_pos, std::ios::beg);
        }
    } else {
        throw std::runtime_error("ByteSequenceFingerprint: non-seekable stream is not supported");
    }
}

ByteSequenceFingerprint::ByteSequenceFingerprint(std::span<const std::byte> data)
{
    size_ = data.size();

    if (size_ == 0) {
        fingerprint_.fill(std::byte{ 0 });
        mode_ = FingerprintMode::RAW;
        return;
    }

    if (size_ <= kSmallThreshold) {
        fingerprint_.fill(std::byte{ 0 });
        std::memcpy(fingerprint_.data(), data.data(), size_);
        mode_ = FingerprintMode::RAW;
        return;
    }

    if (size_ <= kMediumThreshold) {
        writeDigestToFingerprint(Hash::sha256(data.data(), size_), fingerprint_);
        mode_ = FingerprintMode::SHA256;
        return;
    }

    computeSampledFromSpan(data, fingerprint_);
    mode_ = FingerprintMode::SAMPLED_SHA256;
}

bool ByteSequenceFingerprint::operator==(const ByteSequenceFingerprint& rhs) const noexcept
{
    if (size_ != rhs.size_) {
        return false;
    }
    if (mode_ != rhs.mode_) {
        return false;
    }
    return std::memcmp(fingerprint_.data(), rhs.fingerprint_.data(), kFingerprintSize) == 0;
}

bool ByteSequenceFingerprint::operator!=(const ByteSequenceFingerprint& rhs) const noexcept
{
    return !(*this == rhs);
}

VN_CRYPTO_NS_END
