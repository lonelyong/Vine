#include <vine/crypto/Pbkdf2.hpp>

#include <stdexcept>

#include <wolfssl/wolfcrypt/pwdbased.h>

#include "DigestOps.hpp"

VN_CRYPTO_NS_BEGIN

namespace
{

} // namespace

std::vector<std::byte> Pbkdf2::derive(Digest algorithm, std::span<const std::byte> password,
                                      std::span<const std::byte> salt, std::uint32_t iterations,
                                      std::size_t length)
{
    if (salt.empty()) {
        throw std::invalid_argument("vn::crypto::Pbkdf2: the salt must not be empty");
    }
    if (iterations == 0) {
        throw std::invalid_argument("vn::crypto::Pbkdf2: the iteration count must be at least 1");
    }
    if (length == 0) {
        throw std::invalid_argument("vn::crypto::Pbkdf2: the output length must be at least 1 byte");
    }

    std::vector<std::byte> out(length);

    const auto* passwordBytes = reinterpret_cast<const unsigned char*>(password.data());
    const auto* saltBytes     = reinterpret_cast<const unsigned char*>(salt.data());
    if (wc_PBKDF2(reinterpret_cast<unsigned char*>(out.data()), passwordBytes, static_cast<int>(password.size()),
                  saltBytes, static_cast<int>(salt.size()), static_cast<int>(iterations), static_cast<int>(out.size()),
                  detail::wolfHashType(algorithm)) != 0) {
        throw std::runtime_error("vn::crypto::Pbkdf2: the derivation failed");
    }

    return out;
}

VN_CRYPTO_NS_END
