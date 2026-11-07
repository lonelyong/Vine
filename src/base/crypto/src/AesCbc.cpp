#include <vine/crypto/AesCbc.hpp>

#include <algorithm>
#include <cstring>
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

/**
 * @brief Builds the PKCS#7 padding for a partial block.
 *
 * @param used Number of bytes already used in the last block, 0..15.
 * @return A block whose every byte is the padding length.
 */
std::array<std::byte, AesCbc::kBlockSize> pkcs7Pad(std::size_t used)
{
    const auto length = static_cast<unsigned char>(AesCbc::kBlockSize - used);
    std::array<std::byte, AesCbc::kBlockSize> block{};
    block.fill(static_cast<std::byte>(length));
    return block;
}

/**
 * @brief Removes PKCS#7 padding, rejecting every pattern the standard does not allow.
 *
 * @param block    The final decrypted block.
 * @param produced The plaintext of the final block, without the padding.
 * @throws std::runtime_error when the padding length is out of range or a padding byte disagrees.
 */
void stripPkcs7(const std::array<std::byte, AesCbc::kBlockSize>& block, std::vector<std::byte>& produced)
{
    const auto padLength = static_cast<std::size_t>(static_cast<unsigned char>(block.back()));
    if (padLength == 0 || padLength > AesCbc::kBlockSize) {
        throw std::runtime_error("vn::crypto::AesCbc: the plaintext padding is malformed");
    }

    for (std::size_t index = AesCbc::kBlockSize - padLength; index < AesCbc::kBlockSize; ++index) {
        if (block[index] != static_cast<std::byte>(padLength)) {
            throw std::runtime_error("vn::crypto::AesCbc: the plaintext padding is malformed");
        }
    }

    produced.insert(produced.end(), block.begin(),
                    block.begin() + static_cast<std::ptrdiff_t>(AesCbc::kBlockSize - padLength));
}

} // namespace

struct AesCbc::Impl
{
    Aes                    aes{};
    std::vector<std::byte> pending;
    bool                   encrypting{ true };
    bool                   initialized{ false };
    bool                   finished{ false };

    /**
     * @brief Initializes the context and installs the key and the IV.
     *
     * @param key        The AES key.
     * @param iv         The initialization vector, exactly one block.
     * @param encrypting True to prepare for encryption.
     * @throws std::runtime_error when the backend rejects the key or the IV.
     */
    void start(std::span<const std::byte> key, std::span<const std::byte> iv, bool encrypting);

    /**
     * @brief Encrypts whole blocks in place into `out`.
     *
     * @param input  Buffer whose front is a whole number of blocks.
     * @param out    Receives the transformed bytes; resized to the block-aligned length.
     * @return Number of bytes consumed.
     * @throws std::runtime_error when the backend rejects the input.
     */
    std::size_t processBlocks(std::span<const std::byte> input, std::vector<std::byte>& out);
};

void AesCbc::Impl::start(std::span<const std::byte> key, std::span<const std::byte> iv, bool cipherEncrypting)
{
    detail::ensureLibraryInitialized();

    aes = Aes{};
    if (wc_AesInit(&aes, nullptr, INVALID_DEVID) != 0) {
        throw std::runtime_error("vn::crypto::AesCbc: the context could not be initialized");
    }
    initialized = true;

    const auto* keyBytes = reinterpret_cast<const unsigned char*>(key.data());
    const auto* ivBytes  = reinterpret_cast<const unsigned char*>(iv.data());
    if (wc_AesSetKey(&aes, keyBytes, static_cast<word32>(key.size()), ivBytes,
                     cipherEncrypting ? AES_ENCRYPTION : AES_DECRYPTION) != 0) {
        throw std::runtime_error("vn::crypto::AesCbc: the key or the IV was rejected");
    }
}

std::size_t AesCbc::Impl::processBlocks(std::span<const std::byte> input, std::vector<std::byte>& out)
{
    const std::size_t blocks = input.size() / kBlockSize;
    const std::size_t taken   = blocks * kBlockSize;
    if (taken == 0) {
        out.clear();
        return 0;
    }

    out.resize(taken);
    auto*       outBytes = reinterpret_cast<unsigned char*>(out.data());
    const auto* inBytes  = reinterpret_cast<const unsigned char*>(input.data());

    const int status = encrypting ? wc_AesCbcEncrypt(&aes, outBytes, inBytes, static_cast<word32>(taken))
                                  : wc_AesCbcDecrypt(&aes, outBytes, inBytes, static_cast<word32>(taken));
    if (status != 0) {
        throw std::runtime_error("vn::crypto::AesCbc: the chunk was rejected");
    }

    return taken;
}

AesCbc::AesCbc(std::unique_ptr<Impl> state)
  : impl(std::move(state))
{
}

AesCbc AesCbc::encryptor(std::span<const std::byte> key, std::span<const std::byte> iv)
{
    if (!isAesKeySize(key.size())) {
        throw std::invalid_argument("vn::crypto::AesCbc: key must be 16, 24 or 32 bytes");
    }
    if (iv.size() != kBlockSize) {
        throw std::invalid_argument("vn::crypto::AesCbc: the IV must be exactly 16 bytes");
    }

    auto state       = std::make_unique<Impl>();
    state->encrypting = true;
    state->start(key, iv, true);
    return AesCbc(std::move(state));
}

AesCbc AesCbc::decryptor(std::span<const std::byte> key, std::span<const std::byte> iv)
{
    if (!isAesKeySize(key.size())) {
        throw std::invalid_argument("vn::crypto::AesCbc: key must be 16, 24 or 32 bytes");
    }
    if (iv.size() != kBlockSize) {
        throw std::invalid_argument("vn::crypto::AesCbc: the IV must be exactly 16 bytes");
    }

    auto state        = std::make_unique<Impl>();
    state->encrypting = false;
    state->start(key, iv, false);
    return AesCbc(std::move(state));
}

AesCbc::~AesCbc()
{
    if (impl && impl->initialized) {
        wc_AesFree(&impl->aes);
    }
}

AesCbc::AesCbc(AesCbc&& other) noexcept
  : impl(std::move(other.impl))
{
}

AesCbc& AesCbc::operator=(AesCbc&& other) noexcept
{
    if (this != &other) {
        if (impl && impl->initialized) {
            wc_AesFree(&impl->aes);
        }
        impl = std::move(other.impl);
    }
    return *this;
}

std::vector<std::byte> AesCbc::update(std::span<const std::byte> data)
{
    if (impl->finished) {
        throw std::logic_error("vn::crypto::AesCbc: update() after finish()");
    }
    if (data.empty()) {
        return {};
    }

    // Join the held-back bytes with the new chunk, then keep whatever does not fill a block.
    std::vector<std::byte> joined;
    joined.reserve(impl->pending.size() + data.size());
    joined.insert(joined.end(), impl->pending.begin(), impl->pending.end());
    joined.insert(joined.end(), data.begin(), data.end());

    const std::size_t whole = joined.size() / kBlockSize * kBlockSize;

    std::vector<std::byte> out;
    if (whole == 0) {
        impl->pending = std::move(joined);
        return out;
    }

    if (impl->encrypting) {
        // Encryption may emit every complete block; the padding is what fixes the tail.
        impl->processBlocks(std::span<const std::byte>(joined).first(whole), out);
    }
    else {
        // Decryption must hold the final block back: it is where the padding lives.
        std::size_t usable = whole;
        if (joined.size() == whole) {
            usable = whole > kBlockSize ? whole - kBlockSize : 0;
        }
        impl->processBlocks(std::span<const std::byte>(joined).first(usable), out);
        impl->pending.assign(joined.begin() + static_cast<std::ptrdiff_t>(usable), joined.end());
        return out;
    }

    impl->pending.assign(joined.begin() + static_cast<std::ptrdiff_t>(whole), joined.end());
    return out;
}

std::vector<std::byte> AesCbc::finish()
{
    if (impl->finished) {
        throw std::logic_error("vn::crypto::AesCbc: finish() called twice");
    }
    impl->finished = true;

    std::vector<std::byte> out;
    if (impl->encrypting) {
        // PKCS#7 always adds between 1 and 16 bytes, so a complete final block still grows by one block.
        const auto padding = pkcs7Pad(impl->pending.size());
        std::vector<std::byte> last = impl->pending;
        last.insert(last.end(), padding.begin(), padding.end());
        impl->processBlocks(last, out);
    }
    else {
        if (impl->pending.empty() || (impl->pending.size() % kBlockSize) != 0) {
            throw std::runtime_error("vn::crypto::AesCbc: the ciphertext is not a whole number of blocks");
        }

        std::vector<std::byte> last;
        impl->processBlocks(impl->pending, last);

        std::array<std::byte, kBlockSize> block{};
        std::copy(last.begin(), last.end(), block.begin());
        stripPkcs7(block, out);
    }

    impl->pending.clear();
    return out;
}

bool AesCbc::isEncrypting() const
{
    return impl->encrypting;
}

std::vector<std::byte> AesCbc::seal(std::span<const std::byte> key, std::span<const std::byte> iv,
                                    std::span<const std::byte> plaintext)
{
    AesCbc encryptorObject = encryptor(key, iv);

    std::vector<std::byte> ciphertext = encryptorObject.update(plaintext);
    std::vector<std::byte> tail       = encryptorObject.finish();
    ciphertext.insert(ciphertext.end(), tail.begin(), tail.end());
    return ciphertext;
}

std::vector<std::byte> AesCbc::open(std::span<const std::byte> key, std::span<const std::byte> iv,
                                    std::span<const std::byte> ciphertext)
{
    if (ciphertext.empty() || (ciphertext.size() % kBlockSize) != 0) {
        throw std::invalid_argument("vn::crypto::AesCbc: the ciphertext must be a non-empty multiple of 16 bytes");
    }

    AesCbc decryptorObject = decryptor(key, iv);

    std::vector<std::byte> plaintext = decryptorObject.update(ciphertext);
    std::vector<std::byte> tail      = decryptorObject.finish();
    plaintext.insert(plaintext.end(), tail.begin(), tail.end());
    return plaintext;
}

VN_CRYPTO_NS_END
