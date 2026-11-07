#include <vine/crypto/Hmac.hpp>

#include <stdexcept>
#include <utility>

#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/memory.h>

#include "DigestOps.hpp"

VN_CRYPTO_NS_BEGIN

struct Hmac::Impl
{
    // Qualified as ::Hmac: the unqualified name is vn::crypto::Hmac, while the backend context is the global Hmac.
    ::Hmac                 hmac{};
    Digest                 algorithm{ Digest::kSha256 };
    bool                   ready{ false };
    bool                   finished{ false };
    std::vector<std::byte> tag;
};

std::vector<std::byte> hmac(Digest algorithm, std::span<const std::byte> key, std::span<const std::byte> data)
{
    Hmac calculation(algorithm, key);
    calculation.update(data);
    return calculation.finish();
}

bool macEquals(std::span<const std::byte> computed, std::span<const std::byte> received)
{
    if (computed.size() != received.size()) {
        return false;
    }

    // The backend compares without an early exit, so the position of the first difference does not leak.
    return wc_ConstantCompare(reinterpret_cast<const byte*>(computed.data()),
                              reinterpret_cast<const byte*>(received.data()),
                              static_cast<int>(computed.size())) == 0;
}

Hmac::Hmac(Digest algorithm, std::span<const std::byte> key)
  : impl(std::make_unique<Impl>())
{
    impl->algorithm = algorithm;

    if (wc_HmacInit(&impl->hmac, nullptr, INVALID_DEVID) != 0) {
        throw std::runtime_error("vn::crypto::Hmac: the context could not be initialized");
    }
    impl->ready = true;

    const auto* keyBytes = reinterpret_cast<const unsigned char*>(key.data());
    if (wc_HmacSetKey(&impl->hmac, detail::wolfHashType(algorithm), keyBytes, static_cast<word32>(key.size())) != 0) {
        wc_HmacFree(&impl->hmac);
        impl->ready = false;
        throw std::runtime_error("vn::crypto::Hmac: the key was rejected");
    }
}

Hmac::Hmac(std::u8string_view digestName, std::span<const std::byte> key)
  : Hmac(digestFromName(digestName), key)
{
}

Hmac::~Hmac()
{
    if (impl && impl->ready) {
        wc_HmacFree(&impl->hmac);
    }
}

Hmac::Hmac(Hmac&& other) noexcept
  : impl(std::move(other.impl))
{
}

Hmac& Hmac::operator=(Hmac&& other) noexcept
{
    if (this != &other) {
        // The moved-from context keeps its keyed state, so it must be released before being overwritten.
        if (impl && impl->ready) {
            wc_HmacFree(&impl->hmac);
        }
        impl = std::move(other.impl);
    }
    return *this;
}

void Hmac::update(const void* data, std::size_t size)
{
    if (size == 0) {
        return;
    }
    if (impl->finished) {
        throw std::logic_error("vn::crypto::Hmac: update() after finish()");
    }

    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t left  = size;
    while (left > 0) {
        const auto chunk = left > 0xFFFFFFFFu ? static_cast<std::size_t>(0xFFFFFFFFu) : left;
        if (wc_HmacUpdate(&impl->hmac, bytes, static_cast<word32>(chunk)) != 0) {
            throw std::runtime_error("vn::crypto::Hmac: update failed");
        }
        bytes += chunk;
        left -= chunk;
    }
}

void Hmac::update(std::span<const std::byte> data)
{
    update(data.data(), data.size());
}

Hmac& Hmac::operator<<(std::span<const std::byte> data)
{
    update(data);
    return *this;
}

Hmac& Hmac::operator<<(std::u8string_view text)
{
    update(text.data(), text.size());
    return *this;
}

std::size_t Hmac::size() const
{
    return digestSize(impl->algorithm);
}

std::vector<std::byte> Hmac::finish()
{
    if (impl->finished) {
        return impl->tag;
    }

    std::vector<std::byte> out(size());
    if (!impl->ready || wc_HmacFinal(&impl->hmac, reinterpret_cast<unsigned char*>(out.data())) != 0) {
        throw std::runtime_error("vn::crypto::Hmac: finalize failed");
    }

    impl->tag      = std::move(out);
    impl->finished = true;
    return impl->tag;
}

Digest Hmac::algorithm() const
{
    return impl->algorithm;
}

VN_CRYPTO_NS_END
