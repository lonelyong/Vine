#include <vine/crypto/Hasher.hpp>

#include <stdexcept>
#include <utility>

#include "DigestOps.hpp"

VN_CRYPTO_NS_BEGIN

struct Hasher::Impl
{
    detail::DigestContext  context;
    Digest                 algorithm{ Digest::kSha256 };
    bool                   ready{ false };
    bool                   finished{ false };
    std::vector<std::byte> result;
};

Hasher::Hasher(Digest algorithm)
  : impl(std::make_unique<Impl>())
{
    impl->algorithm = algorithm;

    if (!detail::digestInit(algorithm, impl->context)) {
        throw std::runtime_error("vn::crypto::Hasher: the backend could not initialize the digest");
    }
    impl->ready = true;
}

Hasher::Hasher(std::u8string_view name)
  : Hasher(digestFromName(name))
{
}

Hasher::~Hasher()
{
    if (impl && impl->ready) {
        detail::digestFree(impl->algorithm, impl->context);
    }
}

Hasher::Hasher(Hasher&& other) noexcept
  : impl(std::move(other.impl))
{
}

Hasher& Hasher::operator=(Hasher&& other) noexcept
{
    if (this != &other) {
        // The moved-from context is still initialized, so it must be released before being overwritten.
        if (impl && impl->ready) {
            detail::digestFree(impl->algorithm, impl->context);
        }
        impl = std::move(other.impl);
    }
    return *this;
}

void Hasher::update(const void* data, std::size_t size)
{
    if (size == 0) {
        return;
    }
    if (impl->finished) {
        throw std::logic_error("vn::crypto::Hasher: update() after finish()");
    }

    if (!detail::digestUpdate(impl->algorithm, impl->context, data, size)) {
        throw std::runtime_error("vn::crypto::Hasher: the backend rejected the input");
    }
}

void Hasher::update(std::span<const std::byte> data)
{
    update(data.data(), data.size());
}

void Hasher::update(const MemoryStream& stream)
{
    update(stream.data(), stream.size());
}

Hasher& Hasher::operator<<(std::span<const std::byte> data)
{
    update(data);
    return *this;
}

Hasher& Hasher::operator<<(const MemoryStream& stream)
{
    update(stream);
    return *this;
}

Hasher& Hasher::operator<<(std::u8string_view text)
{
    update(text.data(), text.size());
    return *this;
}

std::size_t Hasher::size() const
{
    return digestSize(impl->algorithm);
}

std::vector<std::byte> Hasher::finish()
{
    if (impl->finished) {
        return impl->result;
    }

    std::vector<std::byte> out(size());
    const bool             written =
        detail::digestFinal(impl->algorithm, impl->context, reinterpret_cast<unsigned char*>(out.data()));
    // The context is released by digestFinal() whether or not it succeeded.
    impl->ready = false;

    if (!written) {
        throw std::runtime_error("vn::crypto::Hasher: the backend could not finalize the digest");
    }

    impl->result   = std::move(out);
    impl->finished = true;
    return impl->result;
}

Digest Hasher::algorithm() const
{
    return impl->algorithm;
}

VN_CRYPTO_NS_END
