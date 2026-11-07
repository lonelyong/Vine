#include <vine/crypto/AesCtr.hpp>

#include <stdexcept>
#include <utility>

#include "WolfSsl.hpp"

VN_CRYPTO_NS_BEGIN

namespace
{

/**
 * @brief Rejects a key size AES does not define.
 *
 * @param size Key size in bytes.
 * @return True when the size is 16, 24 or 32.
 */
bool isAesKeySize(std::size_t size)
{
    return size == 16 || size == 24 || size == 32;
}

} // namespace

struct AesCtr::Impl
{
    Aes  aes{};
    bool initialized{ false };
    bool finished{ false };
};

AesCtr::AesCtr(std::unique_ptr<Impl> state)
  : impl(std::move(state))
{
}

AesCtr AesCtr::create(std::span<const std::byte> key, std::span<const std::byte> counterBlock)
{
    if (!isAesKeySize(key.size())) {
        throw std::invalid_argument("vn::crypto::AesCtr: key must be 16, 24 or 32 bytes");
    }
    if (counterBlock.size() != kBlockSize) {
        throw std::invalid_argument("vn::crypto::AesCtr: the counter block must be exactly 16 bytes");
    }

    detail::ensureLibraryInitialized();

    auto state = std::make_unique<Impl>();
    if (wc_AesInit(&state->aes, nullptr, INVALID_DEVID) != 0) {
        throw std::runtime_error("vn::crypto::AesCtr: the context could not be initialized");
    }
    state->initialized = true;

    const auto* keyBytes     = reinterpret_cast<const unsigned char*>(key.data());
    const auto* counterBytes = reinterpret_cast<const unsigned char*>(counterBlock.data());
    // CTR has no direction: the mode flag only decides which key schedule gets prepared, and the keystream is the same.
    if (wc_AesSetKeyDirect(&state->aes, keyBytes, static_cast<word32>(key.size()), counterBytes, AES_ENCRYPTION) != 0) {
        throw std::runtime_error("vn::crypto::AesCtr: the key or the counter was rejected");
    }

    return AesCtr(std::move(state));
}

AesCtr::~AesCtr()
{
    if (impl && impl->initialized) {
        wc_AesFree(&impl->aes);
    }
}

AesCtr::AesCtr(AesCtr&& other) noexcept
  : impl(std::move(other.impl))
{
}

AesCtr& AesCtr::operator=(AesCtr&& other) noexcept
{
    if (this != &other) {
        if (impl && impl->initialized) {
            wc_AesFree(&impl->aes);
        }
        impl = std::move(other.impl);
    }
    return *this;
}

std::vector<std::byte> AesCtr::update(std::span<const std::byte> data)
{
    if (impl->finished) {
        throw std::logic_error("vn::crypto::AesCtr: update() after finish()");
    }

    std::vector<std::byte> out(data.size());
    if (data.empty()) {
        return out;
    }

    auto*       outBytes = reinterpret_cast<unsigned char*>(out.data());
    const auto* inBytes  = reinterpret_cast<const unsigned char*>(data.data());
    // The backend keeps the keystream offset across calls, so any chunk size works.
    if (wc_AesCtrEncrypt(&impl->aes, outBytes, inBytes, static_cast<word32>(data.size())) != 0) {
        throw std::runtime_error("vn::crypto::AesCtr: the chunk was rejected");
    }

    return out;
}

std::vector<std::byte> AesCtr::finish()
{
    if (impl->finished) {
        throw std::logic_error("vn::crypto::AesCtr: finish() called twice");
    }
    impl->finished = true;
    return {};
}

std::vector<std::byte> AesCtr::apply(std::span<const std::byte> key, std::span<const std::byte> counterBlock,
                                     std::span<const std::byte> data)
{
    AesCtr cipher = create(key, counterBlock);
    std::vector<std::byte> out = cipher.update(data);
    cipher.finish();
    return out;
}

VN_CRYPTO_NS_END
