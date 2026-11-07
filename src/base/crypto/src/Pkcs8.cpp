#include "Pkcs8.hpp"

#include <climits>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <wolfssl/wolfcrypt/asn.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/memory.h>

#include "WolfSsl.hpp"

VN_CRYPTO_NS_BEGIN

namespace detail
{

namespace
{

/**
 * @brief Views the caller's UTF-8 text as the plain characters the backend takes.
 *
 * @param text The text.
 * @return The same bytes, typed as chars.
 */
std::string_view asChars(std::u8string_view text)
{
    return std::string_view(reinterpret_cast<const char*>(text.data()), text.size());
}

/**
 * @brief Overwrites a buffer that held key material or a password-derived value.
 *
 * @param buffer The buffer to wipe; may be empty.
 */
void wipe(std::vector<std::byte>& buffer)
{
    if (!buffer.empty()) {
        wc_ForceZero(buffer.data(), static_cast<word32>(buffer.size()));
    }
}

} // namespace

std::vector<std::byte> encryptPkcs8Der(std::span<const std::byte> traditional_der, std::u8string_view password,
                                       unsigned iterations)
{
    if (traditional_der.empty()) {
        throw std::invalid_argument("vn::crypto: the DER bytes are empty");
    }
    if (password.empty()) {
        throw std::invalid_argument("vn::crypto: the password must not be empty");
    }
    if (iterations == 0) {
        throw std::invalid_argument("vn::crypto: the iteration count must be at least 1");
    }
    if (iterations > static_cast<unsigned>(INT_MAX)) {
        throw std::invalid_argument("vn::crypto: the iteration count does not fit the backend's counter");
    }

    // The backend takes a mutable pointer because the encoding path reuses the buffer; the caller's bytes stay intact.
    std::vector<std::byte> key(traditional_der.begin(), traditional_der.end());

    RandomHandle rng;
    const auto   text             = asChars(password);
    const int    iterations_count = static_cast<int>(iterations);

    // A null output buffer asks for the encoded length only.
    word32 encoded_size = 0;
    int    result = wc_CreateEncryptedPKCS8Key(reinterpret_cast<unsigned char*>(key.data()),
                                               static_cast<word32>(key.size()), nullptr, &encoded_size,
                                               text.data(), static_cast<int>(text.size()), PKCS5, PBES2,
                                               AES256CBCb, nullptr, 0, iterations_count, rng.get(), nullptr);
    if (result != LENGTH_ONLY_E) {
        wipe(key);
        throw std::runtime_error("vn::crypto: the key could not be encoded as an encrypted PKCS#8 structure");
    }

    std::vector<std::byte> encoded(static_cast<std::size_t>(encoded_size));
    result = wc_CreateEncryptedPKCS8Key(reinterpret_cast<unsigned char*>(key.data()),
                                        static_cast<word32>(key.size()),
                                        reinterpret_cast<unsigned char*>(encoded.data()), &encoded_size,
                                        text.data(), static_cast<int>(text.size()), PKCS5, PBES2, AES256CBCb,
                                        nullptr, 0, iterations_count, rng.get(), nullptr);
    wipe(key);
    if (result <= 0) {
        throw std::runtime_error("vn::crypto: the key could not be encrypted");
    }

    encoded.resize(static_cast<std::size_t>(result));
    return encoded;
}

std::vector<std::byte> decryptPkcs8Der(std::span<const std::byte> encrypted_der, std::u8string_view password)
{
    if (encrypted_der.empty()) {
        throw std::invalid_argument("vn::crypto: the DER bytes are empty");
    }
    if (password.empty()) {
        throw std::invalid_argument("vn::crypto: the password must not be empty");
    }

    // Decryption happens in place: the plaintext is written over the ciphertext, so the input must be a copy.
    std::vector<std::byte> buffer(encrypted_der.begin(), encrypted_der.end());
    const auto             text = asChars(password);

    const int plain_size = wc_DecryptPKCS8Key(reinterpret_cast<unsigned char*>(buffer.data()),
                                              static_cast<word32>(buffer.size()), text.data(),
                                              static_cast<int>(text.size()));
    if (plain_size <= 0) {
        wipe(buffer);
        // A wrong password and a malformed structure are the same failure to the caller: neither yields a key.
        throw std::invalid_argument(
            "vn::crypto: the encrypted private key was rejected (wrong password or unsupported encoding)");
    }

    // What comes back is the PKCS#8 PrivateKeyInfo that was encrypted; the importers want the inner key.
    word32    offset   = 0;
    const int key_size = wc_GetPkcs8TraditionalOffset(reinterpret_cast<unsigned char*>(buffer.data()), &offset,
                                                      static_cast<word32>(plain_size));
    if (key_size <= 0) {
        wipe(buffer);
        throw std::invalid_argument("vn::crypto: the decrypted key is not a PKCS#8 private key");
    }

    const auto             start = buffer.begin() + static_cast<std::ptrdiff_t>(offset);
    std::vector<std::byte> key(start, start + static_cast<std::ptrdiff_t>(key_size));
    wipe(buffer);
    return key;
}

} // namespace detail

VN_CRYPTO_NS_END
