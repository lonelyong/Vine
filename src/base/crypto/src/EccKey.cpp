#include <vine/crypto/EccKey.hpp>

#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/ecc.h>

#include <vine/crypto/Digest.hpp>

#include "Certificate.hpp"
#include "Pem.hpp"
#include "Pkcs8.hpp"
#include "WolfSsl.hpp"

VN_CRYPTO_NS_BEGIN

namespace
{

/// SHA-256 digest width: the signature hash on P-256.
constexpr std::size_t kSha256Size = 32;

/// SHA-384 digest width: the signature hash on P-384.
constexpr std::size_t kSha384Size = 48;

/**
 * @brief Maps the module's curve enum to the backend's curve identifier.
 *
 * @param curve The curve.
 * @return wolfSSL's curve id.
 */
int wolfCurve(EccKey::Curve curve)
{
    return curve == EccKey::Curve::kP256 ? ECC_SECP256R1 : ECC_SECP384R1;
}

/**
 * @brief Returns the curve order width in bytes, which is also the width of one signature part.
 *
 * @param curve The curve.
 * @return 32 for P-256, 48 for P-384.
 */
std::size_t orderSize(EccKey::Curve curve)
{
    return curve == EccKey::Curve::kP256 ? kSha256Size : kSha384Size;
}

/**
 * @brief Recognizes the curve from the backend's reported order size.
 *
 * @param size Size in bytes as reported by `wc_ecc_size`.
 * @return The matching curve.
 * @throws std::runtime_error when the size belongs to no supported curve.
 */
EccKey::Curve curveFromSize(int size)
{
    if (size == static_cast<int>(kSha256Size)) {
        return EccKey::Curve::kP256;
    }
    if (size == static_cast<int>(kSha384Size)) {
        return EccKey::Curve::kP384;
    }
    throw std::runtime_error("vn::crypto::EccKey: the key is not on a supported curve");
}

/**
 * @brief Hashes a message with the digest that matches the curve.
 *
 * @param curve   The curve, which decides the digest.
 * @param message The bytes to hash.
 * @return The digest bytes.
 */
std::vector<std::byte> digestMessage(EccKey::Curve curve, std::span<const std::byte> message)
{
    return digest(curve == EccKey::Curve::kP256 ? Digest::kSha256 : Digest::kSha384, message);
}

} // namespace

struct EccKey::Impl
{
    ecc_key key{};
    bool    initialized{ false };
    bool    has_private{ false };
    Curve   curve{ Curve::kP256 };
};

namespace
{

/**
 * @brief Creates an initialized backend key and reports failure as an exception.
 *
 * @return The initialized key; the caller owns `wc_ecc_free`.
 * @throws std::runtime_error when the backend refuses to initialize.
 */
ecc_key initializeBackendKey()
{
    ecc_key key{};
    if (wc_ecc_init(&key) != 0) {
        throw std::runtime_error("vn::crypto::EccKey: the key context could not be initialized");
    }
    return key;
}

} // namespace

EccKey::EccKey(std::unique_ptr<Impl> state)
  : impl(std::move(state))
{
}

EccKey EccKey::generate(Curve curve)
{
    auto state         = std::make_unique<Impl>();
    state->key         = initializeBackendKey();
    state->initialized = true;
    state->curve       = curve;

    detail::RandomHandle rng;
    if (wc_ecc_make_key_ex(rng.get(), static_cast<int>(orderSize(curve)), &state->key, wolfCurve(curve)) != 0) {
        wc_ecc_free(&state->key);
        throw std::runtime_error("vn::crypto::EccKey: key generation failed");
    }

    state->has_private = true;
    return EccKey(std::move(state));
}

EccKey EccKey::fromPrivateDer(std::span<const std::byte> der)
{
    if (der.empty()) {
        throw std::invalid_argument("vn::crypto::EccKey: the DER bytes are empty");
    }

    auto state         = std::make_unique<Impl>();
    state->key         = initializeBackendKey();
    state->initialized = true;

    word32      index = 0;
    const auto* bytes = reinterpret_cast<const unsigned char*>(der.data());
    if (wc_EccPrivateKeyDecode(bytes, &index, &state->key, static_cast<word32>(der.size())) != 0) {
        wc_ecc_free(&state->key);
        throw std::invalid_argument("vn::crypto::EccKey: the DER bytes are not an EC private key");
    }

    // The public point is optional in a SEC1 key, and the backend leaves it absent when the file omits it; every export
    // of the public half, and ECDH, then fails with a bare backend error. Derive it here, so an imported key behaves
    // exactly like a generated one.
    if (state->key.type == ECC_PRIVATEKEY_ONLY) {
        detail::RandomHandle rng;
        if (wc_ecc_make_pub_ex(&state->key, nullptr, rng.get()) != 0) {
            wc_ecc_free(&state->key);
            throw std::runtime_error("vn::crypto::EccKey: the public key could not be derived from the private key");
        }
    }

    state->curve       = curveFromSize(wc_ecc_size(&state->key));
    state->has_private = true;
    return EccKey(std::move(state));
}

EccKey EccKey::fromPublicX963(std::span<const std::byte> point)
{
    if (point.empty()) {
        throw std::invalid_argument("vn::crypto::EccKey: the encoded point is empty");
    }

    auto state         = std::make_unique<Impl>();
    state->key         = initializeBackendKey();
    state->initialized = true;

    const auto* bytes = reinterpret_cast<const unsigned char*>(point.data());
    if (wc_ecc_import_x963(bytes, static_cast<word32>(point.size()), &state->key) != 0) {
        wc_ecc_free(&state->key);
        throw std::invalid_argument("vn::crypto::EccKey: the encoded point was rejected");
    }

    state->curve = curveFromSize(wc_ecc_size(&state->key));
    return EccKey(std::move(state));
}

EccKey EccKey::fromPublicDer(std::span<const std::byte> der)
{
    if (der.empty()) {
        throw std::invalid_argument("vn::crypto::EccKey: the DER bytes are empty");
    }

    auto state         = std::make_unique<Impl>();
    state->key         = initializeBackendKey();
    state->initialized = true;

    word32      index = 0;
    const auto* bytes = reinterpret_cast<const unsigned char*>(der.data());
    if (wc_EccPublicKeyDecode(bytes, &index, &state->key, static_cast<word32>(der.size())) != 0) {
        wc_ecc_free(&state->key);
        throw std::invalid_argument("vn::crypto::EccKey: the DER bytes are not an EC public key");
    }

    state->curve = curveFromSize(wc_ecc_size(&state->key));
    return EccKey(std::move(state));
}

EccKey EccKey::fromCertificateDer(std::span<const std::byte> der)
{
    return fromPublicDer(detail::certificatePublicKeyDer(der));
}

EccKey EccKey::fromCertificatePem(std::u8string_view pem)
{
    return fromCertificateDer(detail::pemToDer(pem));
}

EccKey EccKey::fromPrivatePem(std::u8string_view pem)
{
    if (detail::pemIsEncryptedPrivateKey(pem) || detail::pemIsLegacyEncrypted(pem)) {
        throw std::invalid_argument("vn::crypto::EccKey: the PEM block is password protected; use fromEncryptedPrivatePem()");
    }
    return fromPrivateDer(detail::pemToDer(pem));
}

EccKey EccKey::fromEncryptedPrivateDer(std::span<const std::byte> der, std::u8string_view password)
{
    return fromPrivateDer(detail::decryptPkcs8Der(der, password));
}

EccKey EccKey::fromEncryptedPrivatePem(std::u8string_view pem, std::u8string_view password)
{
    // The legacy `Proc-Type: 4,ENCRYPTED` form is what `openssl ec -aes128` wrote before PKCS#8; it wraps the same SEC1
    // key, so only the decryption step differs.
    if (detail::pemIsEncryptedPrivateKey(pem)) {
        return fromEncryptedPrivateDer(detail::pemToDer(pem), password);
    }
    if (detail::pemIsLegacyEncrypted(pem)) {
        return fromPrivateDer(detail::pemDecryptLegacy(pem, password));
    }
    throw std::invalid_argument("vn::crypto::EccKey: the PEM block is not a password-protected private key");
}

EccKey EccKey::fromPublicPem(std::u8string_view pem)
{
    return fromPublicDer(detail::pemToDer(pem));
}

EccKey::~EccKey()
{
    if (impl && impl->initialized) {
        wc_ecc_free(&impl->key);
    }
}

EccKey::EccKey(EccKey&& other) noexcept
  : impl(std::move(other.impl))
{
}

EccKey& EccKey::operator=(EccKey&& other) noexcept
{
    if (this != &other) {
        if (impl && impl->initialized) {
            wc_ecc_free(&impl->key);
        }
        impl = std::move(other.impl);
    }
    return *this;
}

EccKey::Curve EccKey::curve() const
{
    return impl->curve;
}

bool EccKey::hasPrivate() const
{
    return impl->has_private;
}

std::vector<std::byte> EccKey::toPrivateDer() const
{
    if (!impl->has_private) {
        throw std::logic_error("vn::crypto::EccKey: a public-only key cannot be exported as a private key");
    }

    // The backend answers the required length when asked with a null buffer.
    const int needed = wc_EccPrivateKeyToDer(const_cast<ecc_key*>(&impl->key), nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error("vn::crypto::EccKey: the key could not be encoded");
    }

    std::vector<std::byte> der(static_cast<std::size_t>(needed));
    const int              written =
        wc_EccPrivateKeyToDer(const_cast<ecc_key*>(&impl->key), reinterpret_cast<unsigned char*>(der.data()),
                             static_cast<word32>(der.size()));
    if (written <= 0) {
        throw std::runtime_error("vn::crypto::EccKey: the key could not be encoded");
    }

    der.resize(static_cast<std::size_t>(written));
    return der;
}

std::vector<std::byte> EccKey::toPublicDer() const
{
    // with_AlgCurve = 1 writes the full SubjectPublicKeyInfo, algorithm and named curve included.
    const int needed = wc_EccPublicKeyDerSize(const_cast<ecc_key*>(&impl->key), 1);
    if (needed <= 0) {
        throw std::runtime_error("vn::crypto::EccKey: the public key could not be encoded");
    }

    std::vector<std::byte> der(static_cast<std::size_t>(needed));
    const int              written =
        wc_EccPublicKeyToDer(const_cast<ecc_key*>(&impl->key), reinterpret_cast<unsigned char*>(der.data()),
                             static_cast<word32>(der.size()), 1);
    if (written <= 0) {
        throw std::runtime_error("vn::crypto::EccKey: the public key could not be encoded");
    }

    der.resize(static_cast<std::size_t>(written));
    return der;
}

std::u8string EccKey::toPrivatePem() const
{
    return detail::derToPem(toPrivateDer(), ECC_PRIVATEKEY_TYPE);
}

std::vector<std::byte> EccKey::toEncryptedPrivateDer(std::u8string_view password, unsigned iterations) const
{
    return detail::encryptPkcs8Der(toPrivateDer(), password, iterations);
}

std::u8string EccKey::toEncryptedPrivatePem(std::u8string_view password, unsigned iterations) const
{
    return detail::derToPem(toEncryptedPrivateDer(password, iterations), PKCS8_ENC_PRIVATEKEY_TYPE);
}

std::u8string EccKey::toPublicPem() const
{
    return detail::derToPem(toPublicDer(), PUBLICKEY_TYPE);
}

std::vector<std::byte> EccKey::publicX963() const
{
    // One byte of leading 0x04 plus two coordinates of `orderSize` each.
    const std::size_t      needed = 1 + 2 * orderSize(impl->curve);
    std::vector<std::byte> point(needed);
    word32                 length = static_cast<word32>(point.size());

    if (wc_ecc_export_x963(const_cast<ecc_key*>(&impl->key), reinterpret_cast<unsigned char*>(point.data()),
                           &length) != 0) {
        throw std::runtime_error("vn::crypto::EccKey: the public key could not be encoded");
    }

    point.resize(length);
    return point;
}

std::vector<std::byte> EccKey::sign(std::span<const std::byte> message) const
{
    if (!impl->has_private) {
        throw std::logic_error("vn::crypto::EccKey: a public-only key cannot sign");
    }

    const auto         digest      = digestMessage(impl->curve, message);
    const auto*        digestBytes = reinterpret_cast<const unsigned char*>(digest.data());

    // Timing-resistant ECC reads the context bound to the key; the headroom covers the DER length bytes.
    std::vector<std::byte> signature(2 * orderSize(impl->curve) + 16);
    word32                 length = static_cast<word32>(signature.size());

    detail::RandomHandle rng;
    wc_ecc_set_rng(&impl->key, rng.get());
    if (wc_ecc_sign_hash(digestBytes, static_cast<word32>(digest.size()),
                         reinterpret_cast<unsigned char*>(signature.data()), &length, rng.get(), &impl->key) != 0) {
        throw std::runtime_error("vn::crypto::EccKey: signing failed");
    }

    signature.resize(length);
    return signature;
}

bool EccKey::verify(std::span<const std::byte> message, std::span<const std::byte> signature) const
{
    if (signature.empty()) {
        return false;
    }

    const auto  digest      = digestMessage(impl->curve, message);
    const auto* digestBytes = reinterpret_cast<const unsigned char*>(digest.data());
    const auto* signatureBytes = reinterpret_cast<const unsigned char*>(signature.data());

    int verified = 0;
    detail::RandomHandle rng;
    wc_ecc_set_rng(const_cast<ecc_key*>(&impl->key), rng.get());
    if (wc_ecc_verify_hash(signatureBytes, static_cast<word32>(signature.size()), digestBytes,
                           static_cast<word32>(digest.size()), &verified, const_cast<ecc_key*>(&impl->key)) != 0) {
        return false;
    }

    return verified == 1;
}

std::vector<std::byte> EccKey::sharedSecret(const EccKey& peer) const
{
    if (!impl->has_private) {
        throw std::logic_error("vn::crypto::EccKey: a public-only key cannot derive a shared secret");
    }
    if (peer.curve() != impl->curve) {
        throw std::invalid_argument("vn::crypto::EccKey: both keys must be on the same curve");
    }

    std::vector<std::byte> secret(orderSize(impl->curve));
    word32                 length = static_cast<word32>(secret.size());

    // The exchange blinds with the context bound to the key, so bind one for this call.
    detail::RandomHandle rng;
    wc_ecc_set_rng(const_cast<ecc_key*>(&impl->key), rng.get());
    if (wc_ecc_shared_secret(const_cast<ecc_key*>(&impl->key), const_cast<ecc_key*>(&peer.impl->key),
                             reinterpret_cast<unsigned char*>(secret.data()), &length) != 0) {
        throw std::runtime_error("vn::crypto::EccKey: the shared secret could not be derived");
    }

    secret.resize(length);
    return secret;
}

VN_CRYPTO_NS_END
