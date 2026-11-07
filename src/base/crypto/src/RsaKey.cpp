#include <vine/crypto/RsaKey.hpp>

#include <array>
#include <stdexcept>
#include <utility>

#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/memory.h>
#include <wolfssl/wolfcrypt/rsa.h>

#include <vine/crypto/Digest.hpp>

#include "Certificate.hpp"
#include "Pem.hpp"
#include "Pkcs8.hpp"
#include "WolfSsl.hpp"

VN_CRYPTO_NS_BEGIN

namespace
{

/// Digest used by both signature schemes and by OAEP; 32 bytes for SHA-256.
constexpr std::size_t kDigestSize = 32;

/// OAEP with SHA-256 costs 66 bytes of padding, so the payload must leave this much room.
constexpr std::size_t kOaepOverhead = 66;

} // namespace

struct RsaKey::Impl
{
    ::RsaKey key{};
    bool     initialized{ false };
    bool     has_private{ false };
};

namespace
{

/**
 * @brief Creates an initialized backend key and reports failure as an exception.
 *
 * @return The initialized key; the caller owns `wc_FreeRsaKey`.
 * @throws std::runtime_error when the backend refuses to initialize.
 */
::RsaKey initializeBackendKey()
{
    ::RsaKey key{};
    if (wc_InitRsaKey(&key, nullptr) != 0) {
        throw std::runtime_error("vn::crypto::RsaKey: the key context could not be initialized");
    }
    return key;
}

} // namespace

RsaKey::RsaKey(std::unique_ptr<Impl> state)
  : impl(std::move(state))
{
}

RsaKey RsaKey::generate(unsigned bits)
{
    if (bits < 2048 || bits > 8192 || (bits % 8) != 0) {
        throw std::invalid_argument("vn::crypto::RsaKey: the modulus size must be a multiple of 8 in [2048, 8192]");
    }

    auto state          = std::make_unique<Impl>();
    state->key          = initializeBackendKey();
    state->initialized  = true;

    detail::RandomHandle rng;
    if (wc_MakeRsaKey(&state->key, static_cast<int>(bits), 65537, rng.get()) != 0) {        wc_FreeRsaKey(&state->key);
        throw std::runtime_error("vn::crypto::RsaKey: key generation failed");
    }

    state->has_private = true;
    return RsaKey(std::move(state));
}

RsaKey RsaKey::fromPrivateDer(std::span<const std::byte> der)
{
    if (der.empty()) {
        throw std::invalid_argument("vn::crypto::RsaKey: the DER bytes are empty");
    }

    auto state         = std::make_unique<Impl>();
    state->key         = initializeBackendKey();
    state->initialized = true;

    word32      index = 0;
    const auto* bytes = reinterpret_cast<const unsigned char*>(der.data());
    if (wc_RsaPrivateKeyDecode(bytes, &index, &state->key, static_cast<word32>(der.size())) != 0) {
        wc_FreeRsaKey(&state->key);
        throw std::invalid_argument("vn::crypto::RsaKey: the DER bytes are not a PKCS#1 private key");
    }

    state->has_private = true;
    return RsaKey(std::move(state));
}

RsaKey RsaKey::fromRawPublic(std::span<const std::byte> modulus, std::span<const std::byte> exponent)
{
    if (modulus.empty() || exponent.empty()) {
        throw std::invalid_argument("vn::crypto::RsaKey: the modulus and the exponent must not be empty");
    }

    auto state         = std::make_unique<Impl>();
    state->key         = initializeBackendKey();
    state->initialized = true;

    const auto* modulusBytes  = reinterpret_cast<const unsigned char*>(modulus.data());
    const auto* exponentBytes = reinterpret_cast<const unsigned char*>(exponent.data());
    if (wc_RsaPublicKeyDecodeRaw(modulusBytes, static_cast<word32>(modulus.size()), exponentBytes,
                                 static_cast<word32>(exponent.size()), &state->key) != 0) {
        wc_FreeRsaKey(&state->key);
        throw std::invalid_argument("vn::crypto::RsaKey: the modulus/exponent pair was rejected");
    }

    return RsaKey(std::move(state));
}

RsaKey RsaKey::fromPublicDer(std::span<const std::byte> der)
{
    if (der.empty()) {
        throw std::invalid_argument("vn::crypto::RsaKey: the DER bytes are empty");
    }

    auto state         = std::make_unique<Impl>();
    state->key         = initializeBackendKey();
    state->initialized = true;

    // The decoder accepts both the bare PKCS#1 structure and SubjectPublicKeyInfo, so one entry point covers both.
    word32      index = 0;
    const auto* bytes = reinterpret_cast<const unsigned char*>(der.data());
    if (wc_RsaPublicKeyDecode(bytes, &index, &state->key, static_cast<word32>(der.size())) != 0) {
        wc_FreeRsaKey(&state->key);
        throw std::invalid_argument("vn::crypto::RsaKey: the DER bytes are not an RSA public key");
    }

    return RsaKey(std::move(state));
}

RsaKey::~RsaKey(){
    if (impl && impl->initialized) {
        wc_FreeRsaKey(&impl->key);
    }
}

RsaKey::RsaKey(RsaKey&& other) noexcept
  : impl(std::move(other.impl))
{
}

RsaKey& RsaKey::operator=(RsaKey&& other) noexcept
{
    if (this != &other) {
        if (impl && impl->initialized) {
            wc_FreeRsaKey(&impl->key);
        }
        impl = std::move(other.impl);
    }
    return *this;
}

unsigned RsaKey::bits() const
{
    const int size = wc_RsaEncryptSize(&impl->key);
    return size > 0 ? static_cast<unsigned>(size) * 8u : 0u;
}

bool RsaKey::hasPrivate() const
{
    return impl->has_private;
}

std::vector<std::byte> RsaKey::toPrivateDer() const
{
    if (!impl->has_private) {
        throw std::logic_error("vn::crypto::RsaKey: a public-only key cannot be exported as a private key");
    }

    // The backend answers the required length when asked for it with an empty buffer.
    const int needed = wc_RsaKeyToDer(const_cast<::RsaKey*>(&impl->key), nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error("vn::crypto::RsaKey: the key could not be exported");
    }

    std::vector<std::byte> der(static_cast<std::size_t>(needed));
    const int written = wc_RsaKeyToDer(const_cast<::RsaKey*>(&impl->key),
                                       reinterpret_cast<unsigned char*>(der.data()),
                                       static_cast<word32>(der.size()));
    if (written <= 0) {
        throw std::runtime_error("vn::crypto::RsaKey: the key could not be exported");
    }

    der.resize(static_cast<std::size_t>(written));
    return der;
}

std::vector<std::byte> RsaKey::toPublicDer() const
{
    // The size query answers the encoded length, SubjectPublicKeyInfo wrapper included.
    const int needed = wc_RsaPublicKeyDerSize(const_cast<::RsaKey*>(&impl->key), 1);
    if (needed <= 0) {
        throw std::runtime_error("vn::crypto::RsaKey: the public key could not be encoded");
    }

    std::vector<std::byte> der(static_cast<std::size_t>(needed));
    const int              written =
        wc_RsaKeyToPublicDer(const_cast<::RsaKey*>(&impl->key), reinterpret_cast<unsigned char*>(der.data()),
                             static_cast<word32>(der.size()));
    if (written <= 0) {
        throw std::runtime_error("vn::crypto::RsaKey: the public key could not be encoded");
    }

    der.resize(static_cast<std::size_t>(written));
    return der;
}

std::u8string RsaKey::toPrivatePem() const
{
    return detail::derToPem(toPrivateDer(), PRIVATEKEY_TYPE);
}

std::vector<std::byte> RsaKey::toEncryptedPrivateDer(std::u8string_view password, unsigned iterations) const
{
    return detail::encryptPkcs8Der(toPrivateDer(), password, iterations);
}

std::u8string RsaKey::toEncryptedPrivatePem(std::u8string_view password, unsigned iterations) const
{
    return detail::derToPem(toEncryptedPrivateDer(password, iterations), PKCS8_ENC_PRIVATEKEY_TYPE);
}

RsaKey RsaKey::fromCertificateDer(std::span<const std::byte> der)
{
    // The extracted structure is a SubjectPublicKeyInfo, which the public-key importer already accepts; a certificate
    // holding another kind of key is therefore rejected by that importer, with a message naming the real problem.
    return fromPublicDer(detail::certificatePublicKeyDer(der));
}

RsaKey RsaKey::fromCertificatePem(std::u8string_view pem)
{
    return fromCertificateDer(detail::pemToDer(pem));
}

RsaKey RsaKey::fromPrivatePem(std::u8string_view pem)
{
    if (detail::pemIsEncryptedPrivateKey(pem) || detail::pemIsLegacyEncrypted(pem)) {
        throw std::invalid_argument("vn::crypto::RsaKey: the PEM block is password protected; use fromEncryptedPrivatePem()");
    }
    return fromPrivateDer(detail::pemToDer(pem));
}

RsaKey RsaKey::fromEncryptedPrivateDer(std::span<const std::byte> der, std::u8string_view password)
{
    return fromPrivateDer(detail::decryptPkcs8Der(der, password));
}

RsaKey RsaKey::fromEncryptedPrivatePem(std::u8string_view pem, std::u8string_view password)
{
    // Two shapes exist: the PKCS#8 `ENCRYPTED PRIVATE KEY` block, and the older `Proc-Type: 4,ENCRYPTED` headers inside
    // a traditional `RSA PRIVATE KEY` block. Which one it is decides how it gets decrypted, so it is decided here.
    if (detail::pemIsEncryptedPrivateKey(pem)) {
        return fromEncryptedPrivateDer(detail::pemToDer(pem), password);
    }
    if (detail::pemIsLegacyEncrypted(pem)) {
        return fromPrivateDer(detail::pemDecryptLegacy(pem, password));
    }
    throw std::invalid_argument("vn::crypto::RsaKey: the PEM block is not a password-protected private key");
}

RsaKey RsaKey::fromPublicPem(std::u8string_view pem)
{
    return fromPublicDer(detail::pemToDer(pem));
}

std::u8string RsaKey::toPublicPem() const
{
    return detail::derToPem(toPublicDer(), PUBLICKEY_TYPE);
}

std::vector<std::byte> RsaKey::sign(std::span<const std::byte> message, Scheme scheme) const
{
    if (!impl->has_private) {
        throw std::logic_error("vn::crypto::RsaKey: a public-only key cannot sign");
    }

    const auto  messageDigest = ::vn::crypto::digest(Digest::kSha256, message);
    const auto* digestBytes   = reinterpret_cast<const unsigned char*>(messageDigest.data());

    const int modulusSize = wc_RsaEncryptSize(&impl->key);
    if (modulusSize <= 0) {
        throw std::runtime_error("vn::crypto::RsaKey: the key has no usable modulus");
    }

    std::vector<std::byte> signature(static_cast<std::size_t>(modulusSize));
    auto*                  outBytes = reinterpret_cast<unsigned char*>(signature.data());

    detail::RandomHandle rng;
    int       written = 0;
    if (scheme == Scheme::kPss) {
        written = wc_RsaPSS_Sign(digestBytes, static_cast<word32>(kDigestSize), outBytes,
                                 static_cast<word32>(signature.size()), WC_HASH_TYPE_SHA256, WC_MGF1SHA256,
                                 &impl->key, rng.get());
    }
    else {
        written = wc_RsaSSL_Sign(digestBytes, static_cast<word32>(kDigestSize), outBytes,
                                 static_cast<word32>(signature.size()), &impl->key, rng.get());
    }

    if (written <= 0) {
        throw std::runtime_error("vn::crypto::RsaKey: signing failed");
    }

    signature.resize(static_cast<std::size_t>(written));
    return signature;
}

bool RsaKey::verify(std::span<const std::byte> message, std::span<const std::byte> signature, Scheme scheme) const
{
    if (signature.empty()) {
        return false;
    }

    const auto  messageDigest  = ::vn::crypto::digest(Digest::kSha256, message);
    const auto* digestBytes    = reinterpret_cast<const unsigned char*>(messageDigest.data());
    const auto* signatureBytes = reinterpret_cast<const unsigned char*>(signature.data());

    // Verification reads the key but does not own it, so the const_cast is only about the backend's signature.
    auto* key = const_cast<::RsaKey*>(&impl->key);

    // This build enables WC_RSA_BLINDING, and the backend reads key->rng for blinding; binding a context here is
    // what keeps verify/decrypt from failing with no RNG at hand.
    detail::RandomHandle rng;
    wc_RsaSetRNG(key, rng.get());

    if (scheme == Scheme::kPss) {
        // wc_RsaPSS_Verify only *decrypts* the signature and answers the PSS data; the comparison is a second step.
        std::vector<unsigned char> pssData(signature.size());
        const int                  pssSize =
            wc_RsaPSS_Verify(signatureBytes, static_cast<word32>(signature.size()), pssData.data(),
                             static_cast<word32>(pssData.size()), WC_HASH_TYPE_SHA256, WC_MGF1SHA256, key);
        if (pssSize <= 0) {
            return false;
        }

        return wc_RsaPSS_CheckPadding(digestBytes, static_cast<word32>(kDigestSize), pssData.data(),
                                      static_cast<word32>(pssSize), WC_HASH_TYPE_SHA256) == 0;
    }

    std::array<unsigned char, kDigestSize> recovered{};
    const int                              recoveredSize =
        wc_RsaSSL_Verify(signatureBytes, static_cast<word32>(signature.size()), recovered.data(),
                         static_cast<word32>(recovered.size()), key);
    if (recoveredSize != static_cast<int>(kDigestSize)) {
        return false;
    }

    // Constant-time: the backend compares every byte, so the position of the first difference does not leak.
    return wc_ConstantCompare(recovered.data(), digestBytes, static_cast<int>(kDigestSize)) == 0;
}

std::vector<std::byte> RsaKey::encrypt(std::span<const std::byte> plaintext) const
{
    const int modulusSize = wc_RsaEncryptSize(&impl->key);
    if (modulusSize <= 0) {
        throw std::runtime_error("vn::crypto::RsaKey: the key has no usable modulus");
    }
    if (plaintext.empty()) {
        throw std::invalid_argument("vn::crypto::RsaKey: the payload is empty");
    }

    const auto capacity = static_cast<std::size_t>(modulusSize);
    if (plaintext.size() + kOaepOverhead > capacity) {
        throw std::invalid_argument("vn::crypto::RsaKey: the payload does not fit in one OAEP block");
    }

    std::vector<std::byte> ciphertext(capacity);
    detail::RandomHandle              rng;
    // OAEP explicitly: wc_RsaPublicEncrypt() defaults to PKCS#1 v1.5 padding, which is not what this API promises.
    const int written =
        wc_RsaPublicEncrypt_ex(reinterpret_cast<const unsigned char*>(plaintext.data()),
                               static_cast<word32>(plaintext.size()),
                               reinterpret_cast<unsigned char*>(ciphertext.data()),
                               static_cast<word32>(ciphertext.size()), &impl->key, rng.get(), WC_RSA_OAEP_PAD,
                               WC_HASH_TYPE_SHA256, WC_MGF1SHA256, nullptr, 0);
    if (written <= 0) {
        throw std::runtime_error("vn::crypto::RsaKey: encryption failed");
    }

    ciphertext.resize(static_cast<std::size_t>(written));
    return ciphertext;
}

std::vector<std::byte> RsaKey::decrypt(std::span<const std::byte> ciphertext) const
{
    if (!impl->has_private) {
        throw std::logic_error("vn::crypto::RsaKey: a public-only key cannot decrypt");
    }
    if (ciphertext.empty()) {
        throw std::invalid_argument("vn::crypto::RsaKey: the ciphertext is empty");
    }

    std::vector<std::byte> plaintext(ciphertext.size());
    detail::RandomHandle              rng;
    // The private operation blinds with key->rng, so the context must be bound before the call.
    auto* key = const_cast<::RsaKey*>(&impl->key);
    wc_RsaSetRNG(key, rng.get());

    const int written =
        wc_RsaPrivateDecrypt_ex(reinterpret_cast<const unsigned char*>(ciphertext.data()),
                                static_cast<word32>(ciphertext.size()),
                                reinterpret_cast<unsigned char*>(plaintext.data()),
                                static_cast<word32>(plaintext.size()), key, WC_RSA_OAEP_PAD, WC_HASH_TYPE_SHA256,
                                WC_MGF1SHA256, nullptr, 0);
    if (written < 0) {
        throw std::runtime_error("vn::crypto::RsaKey: the ciphertext was rejected");
    }

    plaintext.resize(static_cast<std::size_t>(written));
    return plaintext;
}

VN_CRYPTO_NS_END
