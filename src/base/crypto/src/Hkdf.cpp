#include <vine/crypto/Hkdf.hpp>

#include <stdexcept>

#include <wolfssl/wolfcrypt/hmac.h>

#include "DigestOps.hpp"

VN_CRYPTO_NS_BEGIN

namespace
{

/// RFC 5869 caps the output at 255 blocks of the digest output.
constexpr std::size_t kMaxBlocks = 255;

/**
 * @brief Rejects an output length that RFC 5869 does not allow.
 *
 * @param algorithm The digest, which sets the block size.
 * @param length    The requested output length in bytes.
 * @throws std::invalid_argument when the length is zero or above `255 * digestSize`.
 */
void checkLength(Digest algorithm, std::size_t length)
{
    const std::size_t limit = kMaxBlocks * digestSize(algorithm);
    if (length == 0 || length > limit) {
        throw std::invalid_argument("vn::crypto::Hkdf: the output length must be 1..255 * digestSize");
    }
}

} // namespace

std::vector<std::byte> Hkdf::extract(Digest algorithm, std::span<const std::byte> salt,
                                    std::span<const std::byte> ikm)
{
    std::vector<std::byte> prk(digestSize(algorithm));

    const auto* saltBytes = reinterpret_cast<const unsigned char*>(salt.data());
    const auto* ikmBytes  = reinterpret_cast<const unsigned char*>(ikm.data());
    if (wc_HKDF_Extract(detail::wolfHashType(algorithm), saltBytes, static_cast<word32>(salt.size()), ikmBytes,
                        static_cast<word32>(ikm.size()), reinterpret_cast<unsigned char*>(prk.data())) != 0) {
        throw std::runtime_error("vn::crypto::Hkdf: the extract step failed");
    }

    return prk;
}

std::vector<std::byte> Hkdf::expand(Digest algorithm, std::span<const std::byte> prk,
                                    std::span<const std::byte> info, std::size_t length)
{
    checkLength(algorithm, length);

    std::vector<std::byte> out(length);
    const auto*            prkBytes  = reinterpret_cast<const unsigned char*>(prk.data());
    const auto*            infoBytes = reinterpret_cast<const unsigned char*>(info.data());
    if (wc_HKDF_Expand(detail::wolfHashType(algorithm), prkBytes, static_cast<word32>(prk.size()), infoBytes,
                       static_cast<word32>(info.size()), reinterpret_cast<unsigned char*>(out.data()),
                       static_cast<word32>(out.size())) != 0) {
        throw std::runtime_error("vn::crypto::Hkdf: the expand step failed");
    }

    return out;
}

std::vector<std::byte> Hkdf::derive(Digest algorithm, std::span<const std::byte> ikm,
                                   std::span<const std::byte> salt, std::span<const std::byte> info,
                                   std::size_t length)
{
    checkLength(algorithm, length);

    std::vector<std::byte> out(length);
    const auto*            ikmBytes  = reinterpret_cast<const unsigned char*>(ikm.data());
    const auto*            saltBytes = reinterpret_cast<const unsigned char*>(salt.data());
    const auto*            infoBytes = reinterpret_cast<const unsigned char*>(info.data());
    if (wc_HKDF(detail::wolfHashType(algorithm), ikmBytes, static_cast<word32>(ikm.size()), saltBytes,
                static_cast<word32>(salt.size()), infoBytes, static_cast<word32>(info.size()),
                reinterpret_cast<unsigned char*>(out.data()), static_cast<word32>(out.size())) != 0) {
        throw std::runtime_error("vn::crypto::Hkdf: the derivation failed");
    }

    return out;
}

VN_CRYPTO_NS_END
