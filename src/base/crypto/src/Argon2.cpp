#include <vine/crypto/Argon2.hpp>

#include <stdexcept>

#include <wolfssl/wolfcrypt/argon2.h>

VN_CRYPTO_NS_BEGIN

namespace
{

/// RFC 9106 requires at least 8 bytes of salt.
constexpr std::size_t kMinSaltSize = 8;

/// The backend refuses shorter tags; RFC 9106's own minimum.
constexpr std::size_t kMinOutputSize = 4;

/**
 * @brief Maps the module's variant to the backend's type id.
 *
 * @param variant The variant.
 * @return wolfSSL's `WC_ARGON2_*` value.
 */
int wolfVariant(Argon2::Variant variant)
{
    switch (variant) {
    case Argon2::Variant::kD: return WC_ARGON2_D;
    case Argon2::Variant::kI: return WC_ARGON2_I;
    case Argon2::Variant::kId: break;
    }
    return WC_ARGON2_ID;
}

} // namespace

std::vector<std::byte> Argon2::derive(Variant variant, std::span<const std::byte> password,
                                      std::span<const std::byte> salt, std::uint32_t parallelism,
                                      std::uint32_t memory, std::uint32_t iterations, std::size_t length)
{
    if (salt.size() < kMinSaltSize) {
        throw std::invalid_argument("vn::crypto::Argon2: the salt must be at least 8 bytes");
    }
    if (parallelism == 0) {
        throw std::invalid_argument("vn::crypto::Argon2: the lane count must be at least 1");
    }
    if (memory < 8 * parallelism) {
        throw std::invalid_argument("vn::crypto::Argon2: the memory cost must be at least 8 KiB per lane");
    }
    if (iterations == 0) {
        throw std::invalid_argument("vn::crypto::Argon2: the pass count must be at least 1");
    }
    if (length < kMinOutputSize) {
        throw std::invalid_argument("vn::crypto::Argon2: the output length must be at least 4 bytes");
    }

    std::vector<std::byte> out(length);

    const auto* passwordBytes = reinterpret_cast<const unsigned char*>(password.data());
    const auto* saltBytes     = reinterpret_cast<const unsigned char*>(salt.data());
    if (wc_Argon2(wolfVariant(variant), reinterpret_cast<unsigned char*>(out.data()), static_cast<word32>(out.size()),
                  passwordBytes, static_cast<word32>(password.size()), saltBytes, static_cast<word32>(salt.size()),
                  parallelism, memory, iterations) != 0) {
        throw std::runtime_error("vn::crypto::Argon2: the derivation failed");
    }

    return out;
}

VN_CRYPTO_NS_END
