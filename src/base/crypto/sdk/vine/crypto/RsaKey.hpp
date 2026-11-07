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
 * @brief An RSA key pair used for signatures and for sealing short secrets.
 *
 * @details
 * - One object is one key: a generated or imported private key can do everything, a public-only key can verify and
 *   encrypt but not sign or decrypt.
 * - Signatures take a message, hash it with SHA-256 and sign the digest; the two schemes differ only in padding:
 *   PSS (default, randomized) and PKCS#1 v1.5 (needed to interoperate with older peers).
 * - Encryption is RSA-OAEP with SHA-256. It is meant for a session key or a password, not for bulk data: the payload
 *   must fit in `keySize - 66` bytes, and anything longer belongs in AES-GCM under a sealed key.
 * - Key generation is the only slow operation (seconds at 3072 bits in a debug build); everything else is fast.
 *
 * @warning
 * - A public-only key fails at `sign()` and `decrypt()`; the exception says so instead of producing a wrong answer.
 * - One key object belongs to one thread. The backend binds the per-operation random context into the key before
 *   behind-the-scenes blinding, so two threads using the same `RsaKey` would race on that field; use one key per
 *   thread, or serialize the calls.
 * - OAEP and PKCS#1 v1.5 ciphertexts are not interchangeable, and neither are the two signature schemes.
 * - Key material is freed by the backend but not guaranteed to be wiped; keys that must not outlive the process
 *   belong in a key store, not in a value.
 */
class VN_CRYPTO_API RsaKey {
  public:
    /** @brief Signature padding schemes; the digest is SHA-256 either way. */
    enum class Scheme
    {
        kPss,    ///< RSASSA-PSS: randomized, the modern choice.
        kPkcs1,  ///< RSASSA-PKCS#1 v1.5: deterministic, needed for old peers.
    };

    /**
     * @brief Generates a new key pair with the public exponent 65537.
     *
     * @param bits Modulus size in bits; 2048 is the floor, 3072 the recommended default.
     * @return The generated key pair.
     * @throws std::invalid_argument when `bits` is not a multiple of 8 between 2048 and 8192.
     * @throws std::runtime_error when generation fails.
     */
    static RsaKey generate(unsigned bits = 3072);

    /**
     * @brief Imports a private key from a DER-encoded PKCS#1 RSAPrivateKey.
     *
     * @param der The DER bytes.
     * @return The imported key pair.
     * @throws std::invalid_argument when the bytes are not a private key.
     */
    static RsaKey fromPrivateDer(std::span<const std::byte> der);

    /**
     * @brief Imports a public key from its modulus and exponent.
     *
     * @details
     * - This is the shape a certificate or a JWK hands over after parsing: two big-endian integers, no ASN.1.
     *
     * @param modulus  Big-endian modulus `n`.
     * @param exponent Big-endian public exponent `e`, usually `0x010001`.
     * @return The public-only key.
     * @throws std::invalid_argument when either value is empty or rejected by the backend.
     */
    static RsaKey fromRawPublic(std::span<const std::byte> modulus, std::span<const std::byte> exponent);

    /**
     * @brief Imports a public key from a DER-encoded RSAPublicKey or SubjectPublicKeyInfo.
     *
     * @details
     * - Both shapes are accepted because other tools emit both: the bare PKCS#1 structure and the X.509
     *   SubjectPublicKeyInfo that `openssl rsa -pubout` writes. `toPublicDer()` writes the latter.
     *
     * @param der The DER bytes.
     * @return The public-only key.
     * @throws std::invalid_argument when the bytes are not a public key.
     */
    static RsaKey fromPublicDer(std::span<const std::byte> der);

    ~RsaKey();

    /**
     * @brief Moves the key state; the moved-from object must not be used again.
     *
     * @param other The object to move from.
     */
    RsaKey(RsaKey&& other) noexcept;

    RsaKey& operator=(RsaKey&& other) noexcept;

    RsaKey(const RsaKey&)            = delete;
    RsaKey& operator=(const RsaKey&) = delete;

    /**
     * @brief Returns the modulus size in bits.
     *
     * @return The key size, for example 3072.
     */
    unsigned bits() const;

    /**
     * @brief Returns whether this object holds the private half.
     *
     * @return True when `sign()` and `decrypt()` can be used.
     */
    bool hasPrivate() const;

    /**
     * @brief Exports the private key as a DER-encoded PKCS#1 RSAPrivateKey.
     *
     * @return The DER bytes.
     * @throws std::logic_error   when the key is public-only.
     * @throws std::runtime_error when the backend cannot encode it.
     */
    std::vector<std::byte> toPrivateDer() const;

    /**
     * @brief Exports the private key as PEM text.
     *
     * @return The `RSA PRIVATE KEY` block, newline terminated (PKCS#1, unencrypted).
     * @throws std::logic_error   when the key is public-only.
     * @throws std::runtime_error when the backend cannot encode it.
     */
    std::u8string toPrivatePem() const;

    /**
     * @brief Exports the private key as a DER-encoded encrypted PKCS#8 `EncryptedPrivateKeyInfo`.
     *
     * @details
     * - The key is protected with PBES2: AES-256-CBC under a PBKDF2 key, with a random salt and IV.
     * - The iteration count travels inside the structure, so raising it only affects files written from now on; a
     *   reader needs nothing but the password.
     *
     * @warning
     * - The password is the only protection; this is a file format, not a key store. Nothing rate-limits an attacker
     *   who has the file, so a guessable password is as good as no password.
     *
     * @param password   The password to protect the key with; must not be empty.
     * @param iterations PBKDF2 iteration count; defaults to 200000 (roughly a tenth of a second on a desktop).
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
     * @brief Imports the public key out of a DER-encoded X.509 certificate.
     *
     * @details
     * - Only the key is taken: the signature on the certificate, its validity window and its issuer are not checked,
     *   because deciding whether to trust them is the caller's policy, not an algorithm.
     * - Only the first certificate of a bundle is read.
     *
     * @param der The DER bytes of the certificate.
     * @return The public-only key the certificate vouches for.
     * @throws std::invalid_argument when the bytes are not an X.509 certificate, or its key is not an RSA key.
     */
    static RsaKey fromCertificateDer(std::span<const std::byte> der);

    /**
     * @brief Imports the public key out of a PEM-armoured X.509 certificate.
     *
     * @param pem PEM block with the `CERTIFICATE` label.
     * @return The public-only key the certificate vouches for.
     * @throws std::invalid_argument when the text is not a certificate, or its key is not an RSA key.
     */
    static RsaKey fromCertificatePem(std::u8string_view pem);

    /**
     * @brief Imports a private key from PEM text.
     *
     * @param pem PEM block with the `RSA PRIVATE KEY` label.
     * @return The imported key pair.
     * @throws std::invalid_argument when the text is not a supported private key.
     */
    static RsaKey fromPrivatePem(std::u8string_view pem);

    /**
     * @brief Imports a password-protected private key from PEM text.
     *
     * @details
     * - Reads the `ENCRYPTED PRIVATE KEY` block that `toEncryptedPrivatePem()` and `openssl pkcs8 -topk8` write.
     * - The cipher, the KDF and its parameters come from the block, so a file produced by another tool is accepted
     *   as long as the password is right; only PBES2 / PBES1 / PKCS#12 password schemes are understood.
     *
     * @warning
     * - A wrong password and a damaged file throw the same exception: neither discloses which one it was.
     *
     * @param pem      PEM block with the `ENCRYPTED PRIVATE KEY` label.
     * @param password The password the key was protected with; must not be empty.
     * @return The imported key pair.
     * @throws std::invalid_argument when the password is empty, wrong, or the block is not a supported encrypted key.
     */
    static RsaKey fromEncryptedPrivatePem(std::u8string_view pem, std::u8string_view password);

    /**
     * @brief Imports a password-protected private key from a DER-encoded PKCS#8 `EncryptedPrivateKeyInfo`.
     *
     * @details
     * - The DER form is what a key store or a `PKCS#12` bag holds; the PEM form is the same bytes in armour.
     *
     * @param der      The DER bytes of the `EncryptedPrivateKeyInfo`.
     * @param password The password the key was protected with; must not be empty.
     * @return The imported key pair.
     * @throws std::invalid_argument when the password is empty, wrong, or the bytes are not a supported encrypted key.
     */
    static RsaKey fromEncryptedPrivateDer(std::span<const std::byte> der, std::u8string_view password);

    /**
     * @brief Imports a public key from PEM text.
     *
     * @param pem PEM block with the `PUBLIC KEY` label.
     * @return The public-only key.
     * @throws std::invalid_argument when the text is not a supported public key.
     */
    static RsaKey fromPublicPem(std::u8string_view pem);

    /**
     * @brief Exports the public key as PEM text.
     *
     * @return The `PUBLIC KEY` block, newline terminated (SubjectPublicKeyInfo).
     * @throws std::runtime_error when the backend cannot encode it.
     */
    std::u8string toPublicPem() const;

    /**
     * @brief Exports the public key as a DER-encoded SubjectPublicKeyInfo.
     *
     * @details
     * - This is the shape `openssl rsa -pubout` writes and what a certificate body carries, so a verifier can hold it
     *   on its own and never see the private half.
     *
     * @return The DER bytes.
     * @throws std::runtime_error when the backend cannot encode it.
     */
    std::vector<std::byte> toPublicDer() const;

    /**
     * @brief Signs a message.
     *
     * @details
     * - The message is hashed with SHA-256; the signature length equals the modulus size.
     *
     * @param message The bytes to sign.
     * @param scheme  Padding scheme; defaults to PSS.
     * @return The signature.
     * @throws std::logic_error   when the key is public-only.
     * @throws std::runtime_error when signing fails.
     */
    std::vector<std::byte> sign(std::span<const std::byte> message, Scheme scheme = Scheme::kPss) const;

    /**
     * @brief Verifies a signature over a message.
     *
     * @param message   The bytes that were signed.
     * @param signature The signature to check.
     * @param scheme    Padding scheme; must match the one used to sign.
     * @return True when the signature is valid for this key and message; false for any mismatch.
     */
    bool verify(std::span<const std::byte> message, std::span<const std::byte> signature,
                Scheme scheme = Scheme::kPss) const;

    /**
     * @brief Encrypts a short payload with RSA-OAEP (SHA-256).
     *
     * @param plaintext At most `keySize - 66` bytes.
     * @return The ciphertext; its length equals the modulus size.
     * @throws std::invalid_argument when the payload is empty or too long.
     * @throws std::runtime_error when encryption fails.
     */
    std::vector<std::byte> encrypt(std::span<const std::byte> plaintext) const;

    /**
     * @brief Decrypts an RSA-OAEP ciphertext.
     *
     * @param ciphertext A ciphertext produced by `encrypt()` under this key.
     * @return The plaintext.
     * @throws std::logic_error   when the key is public-only.
     * @throws std::runtime_error when the ciphertext is rejected.
     */
    std::vector<std::byte> decrypt(std::span<const std::byte> ciphertext) const;

  private:
    struct Impl;

    /**
     * @brief Adopts an initialized backend key.
     *
     * @param state The state built by one of the factories.
     */
    explicit RsaKey(std::unique_ptr<Impl> state);

    std::unique_ptr<Impl> impl;
};

VN_CRYPTO_NS_END
