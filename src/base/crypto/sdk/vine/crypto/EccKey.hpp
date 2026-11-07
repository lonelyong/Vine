#pragma once

#include "crypto_global.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

VN_CRYPTO_NS_BEGIN

/**
 * @brief An elliptic-curve key pair used for ECDSA signatures and ECDH key agreement.
 *
 * @details
 * - One object is one key on one named curve: generated or imported keys hold the private half, imported public points
 *   hold only the public half.
 * - Signatures take a message, hash it (SHA-256 on P-256, SHA-384 on P-384), and answer the DER-encoded
 *   ECDSA-Sig-Value `SEQUENCE { r, s }` — the format openssl and every certificate tool speaks.
 * - `sharedSecret()` is ECDH: both sides arrive at the same bytes, which are key material and must not be used as a
 *   key directly — feed them to `Hkdf::derive()` together with a purpose-specific `info`.
 * - Key generation is cheap compared with RSA, so P-256 is a reasonable default for anything new.
 *
 * @warning
 * - A public-only key fails at `sign()` and `sharedSecret()`; the exception says so instead of producing a wrong answer.
 * - One key object belongs to one thread: verification and the key exchange bind the per-operation random context into
 *   the key, so two threads sharing one `EccKey` would race on that field.
 * - ECDSA also needs a good per-signature nonce: it comes from the backend's RNG, which is why a `RandomHandle` is
 *   created for every signature. Never replace it with a caller-supplied nonce unless you know why that is dangerous.
 * - A signature's length varies with its contents (70–72 bytes on P-256, 102–104 on P-384): treat it as a byte blob, not
 *   as two fixed-size halves, and never assume a fixed size when sizing a wire format.
 */
class VN_CRYPTO_API EccKey {
  public:
    /** @brief The curves this module supports. */
    enum class Curve
    {
        kP256,  ///< NIST P-256 (secp256r1), 256-bit order.
        kP384,  ///< NIST P-384 (secp384r1), 384-bit order.
    };

    /**
     * @brief Generates a new key pair on the given curve.
     *
     * @param curve The curve; P-256 by default.
     * @return The generated key pair.
     * @throws std::runtime_error when the backend cannot generate the key.
     */
    static EccKey generate(Curve curve = Curve::kP256);

    /**
     * @brief Imports a private key from a DER-encoded SEC1 ECPrivateKey.
     *
     * @param der The DER bytes.
     * @return The imported key pair.
     * @throws std::invalid_argument when the bytes are not a supported private key.
     */
    static EccKey fromPrivateDer(std::span<const std::byte> der);

    /**
     * @brief Imports a public key from an X9.63 uncompressed point.
     *
     * @details
     * - The point is the format other systems publish: `0x04` followed by the big-endian X then Y coordinates.
     *
     * @param point The encoded point.
     * @return The public-only key.
     * @throws std::invalid_argument when the point is empty or not on a supported curve.
     */
    static EccKey fromPublicX963(std::span<const std::byte> point);

    /**
     * @brief Imports a public key from a DER-encoded SubjectPublicKeyInfo.
     *
     * @param der The DER bytes.
     * @return The public-only key.
     * @throws std::invalid_argument when the bytes are not an EC public key.
     */
    static EccKey fromPublicDer(std::span<const std::byte> der);

    /**
     * @brief Imports the public key out of a DER-encoded X.509 certificate.
     *
     * @details
     * - Only the key is taken: the signature on the certificate, its validity window and its issuer are not checked,
     *   because deciding whether to trust them is the caller's policy, not an algorithm.
     * - Only the first certificate of a bundle is read.
     *
     * @param der The DER bytes of the certificate.
     * @return The public-only key the certificate vouches for.
     * @throws std::invalid_argument when the bytes are not an X.509 certificate, or its key is not an ECC key.
     */
    static EccKey fromCertificateDer(std::span<const std::byte> der);

    /**
     * @brief Imports the public key out of a PEM-armoured X.509 certificate.
     *
     * @param pem PEM block with the `CERTIFICATE` label.
     * @return The public-only key the certificate vouches for.
     * @throws std::invalid_argument when the text is not a certificate, or its key is not an ECC key.
     */
    static EccKey fromCertificatePem(std::u8string_view pem);

    /**
     * @brief Imports a private key from PEM text.
     *
     * @param pem PEM block with the `EC PRIVATE KEY` label.
     * @return The imported key pair.
     * @throws std::invalid_argument when the text is not a supported private key.
     */
    static EccKey fromPrivatePem(std::u8string_view pem);

    /**
     * @brief Imports a password-protected private key from PEM text.
     *
     * @details
     * - Reads the `ENCRYPTED PRIVATE KEY` block that `toEncryptedPrivatePem()` and `openssl pkcs8 -topk8` write; the
     *   curve comes from the decrypted structure, so it does not have to be known in advance.
     *
     * @warning
     * - A wrong password and a damaged file throw the same exception: neither discloses which one it was.
     *
     * @param pem      PEM block with the `ENCRYPTED PRIVATE KEY` label.
     * @param password The password the key was protected with; must not be empty.
     * @return The imported key pair.
     * @throws std::invalid_argument when the password is empty, wrong, or the block is not a supported encrypted key.
     */
    static EccKey fromEncryptedPrivatePem(std::u8string_view pem, std::u8string_view password);

    /**
     * @brief Imports a password-protected private key from a DER-encoded PKCS#8 `EncryptedPrivateKeyInfo`.
     *
     * @param der      The DER bytes of the `EncryptedPrivateKeyInfo`.
     * @param password The password the key was protected with; must not be empty.
     * @return The imported key pair.
     * @throws std::invalid_argument when the password is empty, wrong, or the bytes are not a supported encrypted key.
     */
    static EccKey fromEncryptedPrivateDer(std::span<const std::byte> der, std::u8string_view password);

    /**
     * @brief Imports a public key from PEM text.
     *
     * @param pem PEM block with the `PUBLIC KEY` label.
     * @return The public-only key.
     * @throws std::invalid_argument when the text is not a supported public key.
     */
    static EccKey fromPublicPem(std::u8string_view pem);

    ~EccKey();

    /**
     * @brief Moves the key state; the moved-from object must not be used again.
     *
     * @param other The object to move from.
     */
    EccKey(EccKey&& other) noexcept;

    EccKey& operator=(EccKey&& other) noexcept;

    EccKey(const EccKey&)            = delete;
    EccKey& operator=(const EccKey&) = delete;

    /**
     * @brief Returns the curve this key lives on.
     *
     * @return The curve identifier.
     */
    Curve curve() const;

    /**
     * @brief Returns whether this object holds the private half.
     *
     * @return True when `sign()` and `sharedSecret()` can be used.
     */
    bool hasPrivate() const;

    /**
     * @brief Exports the private key as a DER-encoded SEC1 ECPrivateKey.
     *
     * @return The DER bytes.
     * @throws std::logic_error   when the key is public-only.
     * @throws std::runtime_error when the backend cannot encode it.
     */
    std::vector<std::byte> toPrivateDer() const;

    /**
     * @brief Exports the public key as a DER-encoded SubjectPublicKeyInfo.
     *
     * @return The DER bytes, algorithm and curve identifiers included.
     * @throws std::runtime_error when the backend cannot encode it.
     */
    std::vector<std::byte> toPublicDer() const;

    /**
     * @brief Exports the private key as PEM text.
     *
     * @return The `EC PRIVATE KEY` block, newline terminated.
     * @throws std::logic_error   when the key is public-only.
     * @throws std::runtime_error when the backend cannot encode it.
     */
    std::u8string toPrivatePem() const;

    /**
     * @brief Exports the private key as a DER-encoded encrypted PKCS#8 `EncryptedPrivateKeyInfo`.
     *
     * @details
     * - The key is protected with PBES2: AES-256-CBC under a PBKDF2 key, with a random salt and IV.
     *
     * @warning
     * - The password is the only protection; this is a file format, not a key store. Nothing rate-limits an attacker
     *   who has the file, so a guessable password is as good as no password.
     *
     * @param password   The password to protect the key with; must not be empty.
     * @param iterations PBKDF2 iteration count; defaults to 200000.
     * @return The DER bytes of the `EncryptedPrivateKeyInfo`.
     * @throws std::logic_error   when the key is public-only.
     * @throws std::invalid_argument when the password is empty or `iterations` is 0.
     * @throws std::runtime_error when the backend cannot encode or encrypt the key.
     */
    std::vector<std::byte> toEncryptedPrivateDer(std::u8string_view password, unsigned iterations = 200000) const;

    /**
     * @brief Exports the private key as a password-protected PEM block.
     *
     * @details
     * - Writes the `ENCRYPTED PRIVATE KEY` block, which every current tool reads (`openssl pkcs8 -in ... -passin ...`).
     *
     * @param password   The password to protect the key with; must not be empty.
     * @param iterations PBKDF2 iteration count; see `toEncryptedPrivateDer()`.
     * @return The PEM text, newline terminated.
     * @throws std::logic_error   when the key is public-only.
     * @throws std::invalid_argument when the password is empty or `iterations` is 0.
     * @throws std::runtime_error when the backend cannot encode or encrypt the key.
     */
    std::u8string toEncryptedPrivatePem(std::u8string_view password, unsigned iterations = 200000) const;

    /**
     * @brief Exports the public key as PEM text.
     *
     * @return The `PUBLIC KEY` block, newline terminated.
     * @throws std::runtime_error when the backend cannot encode it.
     */
    std::u8string toPublicPem() const;

    /**
     * @brief Exports the public key as an X9.63 uncompressed point.
     *
     * @return The encoded point: 65 bytes on P-256, 97 on P-384.
     * @throws std::runtime_error when the backend cannot encode it.
     */
    std::vector<std::byte> publicX963() const;

    /**
     * @brief Signs a message with ECDSA.
     *
     * @param message The bytes to sign.
     * @return The DER-encoded ECDSA-Sig-Value; its length varies (70–72 bytes on P-256, 102–104 on P-384).
     * @throws std::logic_error   when the key is public-only.
     * @throws std::runtime_error when signing fails.
     */
    std::vector<std::byte> sign(std::span<const std::byte> message) const;

    /**
     * @brief Verifies an ECDSA signature over a message.
     *
     * @param message   The bytes that were signed.
     * @param signature The DER-encoded ECDSA-Sig-Value produced by `sign()`.
     * @return True when the signature is valid for this key and message; false for any mismatch.
     */
    bool verify(std::span<const std::byte> message, std::span<const std::byte> signature) const;

    /**
     * @brief Derives the ECDH shared secret with a peer's public key.
     *
     * @param peer A key on the same curve; it only needs the public half.
     * @return The shared secret, one curve-order wide (32 or 48 bytes).
     * @throws std::logic_error   when this key is public-only.
     * @throws std::invalid_argument when the curves differ.
     * @throws std::runtime_error when the backend rejects the exchange.
     */
    std::vector<std::byte> sharedSecret(const EccKey& peer) const;

  private:
    struct Impl;

    /**
     * @brief Adopts an initialized backend key.
     *
     * @param state The state built by one of the factories.
     */
    explicit EccKey(std::unique_ptr<Impl> state);

    std::unique_ptr<Impl> impl;
};

VN_CRYPTO_NS_END
