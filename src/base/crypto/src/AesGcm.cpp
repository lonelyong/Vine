#include <vine/crypto/AesGcm.hpp>

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

struct AesGcm::Impl
{
    Aes                             aes{};
    std::vector<std::byte>          iv;
    std::vector<std::byte>          aad;
    std::array<std::byte, kTagSize> tag{};
    bool                            encrypting{ true };
    bool                            initialized{ false };
    bool                            finished{ false };

    /**
     * @brief Initializes the backend context, sets the key and IV, then feeds the associated data.
     *
     * @param key        The AES key.
     * @param iv         The initialization vector.
     * @param aad        The associated data; GCM requires all of it before the first ciphertext byte.
     * @param encrypting True to prepare for encryption.
     * @throws std::runtime_error when the backend rejects the key, the IV or the associated data.
     */
    void start(std::span<const std::byte> key, std::span<const std::byte> iv, std::span<const std::byte> aad,
               bool encrypting);
};

AesGcm::AesGcm(std::unique_ptr<Impl> state)
  : impl(std::move(state))
{
}

namespace
{

} // namespace

void AesGcm::Impl::start(std::span<const std::byte> key, std::span<const std::byte> iv,
                        std::span<const std::byte> aad, bool cipherEncrypting)
{
    detail::ensureLibraryInitialized();

    this->aes = Aes{};
    if (wc_AesInit(&this->aes, nullptr, INVALID_DEVID) != 0) {
        throw std::runtime_error("vn::crypto::AesGcm: the context could not be initialized");
    }
    initialized = true;

    const auto* keyBytes = reinterpret_cast<const unsigned char*>(key.data());
    const auto* ivBytes  = reinterpret_cast<const unsigned char*>(iv.data());
    const int   status   = cipherEncrypting
                             ? wc_AesGcmEncryptInit(&this->aes, keyBytes, static_cast<word32>(key.size()), ivBytes,
                                                    static_cast<word32>(iv.size()))
                             : wc_AesGcmDecryptInit(&this->aes, keyBytes, static_cast<word32>(key.size()), ivBytes,
                                                    static_cast<word32>(iv.size()));
    if (status != 0) {
        throw std::runtime_error("vn::crypto::AesGcm: the key or the IV was rejected");
    }

    if (!aad.empty()) {
        const auto* aadBytes = reinterpret_cast<const unsigned char*>(aad.data());
        const int   aadStatus =
            cipherEncrypting ? wc_AesGcmEncryptUpdate(&this->aes, nullptr, nullptr, 0, aadBytes,
                                                      static_cast<word32>(aad.size()))
                             : wc_AesGcmDecryptUpdate(&this->aes, nullptr, nullptr, 0, aadBytes,
                                                      static_cast<word32>(aad.size()));
        if (aadStatus != 0) {
            throw std::runtime_error("vn::crypto::AesGcm: the associated data was rejected");
        }
    }
}


AesGcm AesGcm::encryptor(std::span<const std::byte> key, std::span<const std::byte> iv, std::span<const std::byte> aad)
{
    if (!isAesKeySize(key.size())) {
        throw std::invalid_argument("vn::crypto::AesGcm: key must be 16, 24 or 32 bytes");
    }
    if (iv.empty()) {
        throw std::invalid_argument("vn::crypto::AesGcm: the IV must not be empty");
    }

    auto state        = std::make_unique<Impl>();
    state->iv         = std::vector<std::byte>(iv.begin(), iv.end());
    state->aad        = std::vector<std::byte>(aad.begin(), aad.end());
    state->encrypting = true;

    state->start(key, iv, aad, true);
    return AesGcm(std::move(state));
}

AesGcm AesGcm::decryptor(std::span<const std::byte> key, std::span<const std::byte> iv, std::span<const std::byte> tag,
                         std::span<const std::byte> aad)
{
    if (!isAesKeySize(key.size())) {
        throw std::invalid_argument("vn::crypto::AesGcm: key must be 16, 24 or 32 bytes");
    }
    if (iv.empty()) {
        throw std::invalid_argument("vn::crypto::AesGcm: the IV must not be empty");
    }
    if (tag.size() != kTagSize) {
        throw std::invalid_argument("vn::crypto::AesGcm: the tag must be 16 bytes");
    }

    auto state        = std::make_unique<Impl>();
    state->iv         = std::vector<std::byte>(iv.begin(), iv.end());
    state->aad        = std::vector<std::byte>(aad.begin(), aad.end());
    std::copy(tag.begin(), tag.end(), state->tag.begin());
    state->encrypting = false;

    state->start(key, iv, aad, false);
    return AesGcm(std::move(state));
}

AesGcm::~AesGcm()
{
    if (impl && impl->initialized) {
        wc_AesFree(&impl->aes);
    }
}

AesGcm::AesGcm(AesGcm&& other) noexcept
  : impl(std::move(other.impl))
{
}

AesGcm& AesGcm::operator=(AesGcm&& other) noexcept
{
    if (this != &other) {
        if (impl && impl->initialized) {
            wc_AesFree(&impl->aes);
        }
        impl = std::move(other.impl);
    }
    return *this;
}

std::vector<std::byte> AesGcm::update(std::span<const std::byte> data)
{
    std::vector<std::byte> out;
    update(data, out);
    return out;
}

void AesGcm::update(std::span<const std::byte> data, std::vector<std::byte>& out)
{
    out.resize(data.size());

    if (impl->finished) {
        throw std::logic_error("vn::crypto::AesGcm: update() after finish()");
    }
    if (data.empty()) {
        return;
    }

    const auto* inBytes  = reinterpret_cast<const unsigned char*>(data.data());
    auto*       outBytes = reinterpret_cast<unsigned char*>(out.data());
    const int   status =
        impl->encrypting ? wc_AesGcmEncryptUpdate(&impl->aes, outBytes, inBytes, static_cast<word32>(data.size()),
                                                  nullptr, 0)
                         : wc_AesGcmDecryptUpdate(&impl->aes, outBytes, inBytes, static_cast<word32>(data.size()),
                                                  nullptr, 0);
    if (status != 0) {
        throw std::runtime_error("vn::crypto::AesGcm: the chunk was rejected");
    }
}

std::vector<std::byte> AesGcm::finish()
{
    if (impl->finished) {
        throw std::logic_error("vn::crypto::AesGcm: finish() called twice");
    }
    impl->finished = true;

    auto* tagBytes = reinterpret_cast<unsigned char*>(impl->tag.data());

    if (impl->encrypting) {
        if (wc_AesGcmEncryptFinal(&impl->aes, tagBytes, static_cast<word32>(kTagSize)) != 0) {
            throw std::runtime_error("vn::crypto::AesGcm: the tag could not be finalized");
        }
    }
    else {
        // The result is the authentication decision: anything but 0 means the ciphertext must be discarded.
        if (wc_AesGcmDecryptFinal(&impl->aes, tagBytes, static_cast<word32>(kTagSize)) != 0) {
            throw std::runtime_error("vn::crypto::AesGcm: the tag does not verify");
        }
    }

    return {};
}

bool AesGcm::isEncrypting() const
{
    return impl->encrypting;
}

std::array<std::byte, AesGcm::kTagSize> AesGcm::tag() const
{
    if (!impl->encrypting) {
        throw std::logic_error("vn::crypto::AesGcm: tag() is only meaningful on an encrypting object");
    }
    if (!impl->finished) {
        throw std::logic_error("vn::crypto::AesGcm: tag() before finish()");
    }

    return impl->tag;
}

AesGcm::Sealed AesGcm::seal(std::span<const std::byte> key, std::span<const std::byte> iv,
                            std::span<const std::byte> plaintext, std::span<const std::byte> aad)
{
    AesGcm encryptorObject = encryptor(key, iv, aad);

    Sealed sealed;
    sealed.ciphertext = encryptorObject.update(plaintext);
    encryptorObject.finish();
    sealed.tag = encryptorObject.tag();
    return sealed;
}

std::vector<std::byte> AesGcm::open(std::span<const std::byte> key, std::span<const std::byte> iv,
                                    std::span<const std::byte> ciphertext, std::span<const std::byte> tag,
                                    std::span<const std::byte> aad)
{
    AesGcm decryptorObject = decryptor(key, iv, tag, aad);

    std::vector<std::byte> plaintext = decryptorObject.update(ciphertext);
    // Throws when the tag does not verify; the caller then drops `plaintext`, which is not authentic.
    decryptorObject.finish();
    return plaintext;
}

VN_CRYPTO_NS_END
