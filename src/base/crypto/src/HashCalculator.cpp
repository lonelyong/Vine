#include <vine/crypto/HashCalculator.hpp>

#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>

#include <wolfssl/wolfcrypt/hash.h>

VN_CRYPTO_NS_BEGIN

namespace
{

/**
 * @brief Maps an algorithm to its wolfSSL hash type id.
 *
 * @param algorithm The algorithm.
 * @return The wolfSSL hash type id.
 */
enum wc_HashType toWolfType(HashCalculator::Algorithm algorithm)
{
    switch (algorithm) {
        case HashCalculator::Algorithm::ALG_MD5: return WC_HASH_TYPE_MD5;
        case HashCalculator::Algorithm::ALG_SHA1: return WC_HASH_TYPE_SHA;
        case HashCalculator::Algorithm::ALG_SHA224: return WC_HASH_TYPE_SHA224;
        case HashCalculator::Algorithm::ALG_SHA256: return WC_HASH_TYPE_SHA256;
        case HashCalculator::Algorithm::ALG_SHA384: return WC_HASH_TYPE_SHA384;
        case HashCalculator::Algorithm::ALG_SHA512: return WC_HASH_TYPE_SHA512;
        case HashCalculator::Algorithm::ALG_SHA3_224: return WC_HASH_TYPE_SHA3_224;
        case HashCalculator::Algorithm::ALG_SHA3_256: return WC_HASH_TYPE_SHA3_256;
        case HashCalculator::Algorithm::ALG_SHA3_384: return WC_HASH_TYPE_SHA3_384;
        case HashCalculator::Algorithm::ALG_SHA3_512: return WC_HASH_TYPE_SHA3_512;
    }
    return WC_HASH_TYPE_MD5;
}

/**
 * @brief Returns the digest size of an algorithm in bytes.
 *
 * @param algorithm The algorithm.
 * @return The digest size.
 */
std::size_t digestSizeFor(HashCalculator::Algorithm algorithm)
{
    switch (algorithm) {
        case HashCalculator::Algorithm::ALG_MD5: return WC_MD5_DIGEST_SIZE;
        case HashCalculator::Algorithm::ALG_SHA1: return WC_SHA_DIGEST_SIZE;
        case HashCalculator::Algorithm::ALG_SHA224: return WC_SHA224_DIGEST_SIZE;
        case HashCalculator::Algorithm::ALG_SHA256: return WC_SHA256_DIGEST_SIZE;
        case HashCalculator::Algorithm::ALG_SHA384: return WC_SHA384_DIGEST_SIZE;
        case HashCalculator::Algorithm::ALG_SHA512: return WC_SHA512_DIGEST_SIZE;
        case HashCalculator::Algorithm::ALG_SHA3_224: return WC_SHA3_224_DIGEST_SIZE;
        case HashCalculator::Algorithm::ALG_SHA3_256: return WC_SHA3_256_DIGEST_SIZE;
        case HashCalculator::Algorithm::ALG_SHA3_384: return WC_SHA3_384_DIGEST_SIZE;
        case HashCalculator::Algorithm::ALG_SHA3_512: return WC_SHA3_512_DIGEST_SIZE;
    }
    return 0;
}

/**
 * @brief Normalizes a name to lowercase with '-' separators.
 *
 * @param name The raw name.
 * @return The normalized name.
 */
std::string normalizeName(std::u8string_view name)
{
    std::string out;
    out.reserve(name.size());
    for (char8_t c : name) {
        char ch = static_cast<char>(c);
        if (ch == '_') {
            ch = '-';
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

/**
 * @brief Parses an algorithm name into an Algorithm value.
 *
 * @param name The algorithm name.
 * @return The parsed algorithm.
 * @throws std::invalid_argument when the name is unknown.
 */
HashCalculator::Algorithm parseAlgorithm(std::u8string_view name)
{
    const std::string normalized = normalizeName(name);
    if (normalized == "md5") {
        return HashCalculator::Algorithm::ALG_MD5;
    }
    if (normalized == "sha1") {
        return HashCalculator::Algorithm::ALG_SHA1;
    }
    if (normalized == "sha224") {
        return HashCalculator::Algorithm::ALG_SHA224;
    }
    if (normalized == "sha256") {
        return HashCalculator::Algorithm::ALG_SHA256;
    }
    if (normalized == "sha384") {
        return HashCalculator::Algorithm::ALG_SHA384;
    }
    if (normalized == "sha512") {
        return HashCalculator::Algorithm::ALG_SHA512;
    }
    if (normalized == "sha3-224") {
        return HashCalculator::Algorithm::ALG_SHA3_224;
    }
    if (normalized == "sha3-256") {
        return HashCalculator::Algorithm::ALG_SHA3_256;
    }
    if (normalized == "sha3-384") {
        return HashCalculator::Algorithm::ALG_SHA3_384;
    }
    if (normalized == "sha3-512") {
        return HashCalculator::Algorithm::ALG_SHA3_512;
    }
    throw std::invalid_argument("unknown hash algorithm: " + normalized);
}

} // namespace

struct HashCalculator::Impl
{
    /**
     * @brief Initializes the wolfSSL hash context.
     *
     * @param algorithm The algorithm.
     * @param type The wolfSSL hash type id.
     * @throws std::runtime_error when wolfSSL initialization fails.
     */
    Impl(Algorithm algorithm, enum wc_HashType type)
        : algorithm(algorithm)
        , type(type)
        , finalized(false)
    {
        std::memset(&hash, 0, sizeof hash);
        if (wc_HashInit(&hash, type) != 0) {
            throw std::runtime_error("HashCalculator: wc_HashInit failed");
        }
    }

    Algorithm                algorithm;
    enum wc_HashType         type;
    bool                     finalized;
    wc_HashAlg               hash;
    std::vector<unsigned char> result;
};

HashCalculator::HashCalculator(Algorithm algorithm)
    : impl(std::make_unique<Impl>(algorithm, toWolfType(algorithm)))
{
}

HashCalculator::HashCalculator(std::u8string_view name)
    : HashCalculator(parseAlgorithm(name))
{
}

HashCalculator::~HashCalculator() = default;

HashCalculator::HashCalculator(HashCalculator&&) noexcept = default;

HashCalculator& HashCalculator::operator=(HashCalculator&&) noexcept = default;

void HashCalculator::write(const void* data, std::size_t size)
{
    if (impl->finalized) {
        throw std::logic_error("HashCalculator: cannot write after digest()");
    }
    // The update function takes a 32-bit length, so feed the input in chunks.
    const auto* p = static_cast<const unsigned char*>(data);
    while (size > 0) {
        const word32 chunk = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<word32>(size);
        if (wc_HashUpdate(&impl->hash, impl->type, p, chunk) != 0) {
            throw std::runtime_error("HashCalculator: wc_HashUpdate failed");
        }
        p += chunk;
        size -= chunk;
    }
}

void HashCalculator::write(const MemoryStream& stream)
{
    write(stream.data(), stream.size());
}

HashCalculator& HashCalculator::operator<<(const MemoryStream& stream)
{
    write(stream);
    return *this;
}

HashCalculator& HashCalculator::operator<<(std::u8string_view text)
{
    write(text.data(), text.size());
    return *this;
}

std::size_t HashCalculator::digestSize() const
{
    return digestSizeFor(impl->algorithm);
}

std::vector<unsigned char> HashCalculator::digest()
{
    if (!impl->finalized) {
        impl->result.assign(digestSize(), 0);
        if (wc_HashFinal(&impl->hash, impl->type, impl->result.data()) != 0) {
            throw std::runtime_error("HashCalculator: wc_HashFinal failed");
        }
        impl->finalized = true;
    }
    return impl->result;
}

HashCalculator::Algorithm HashCalculator::algorithm() const
{
    return impl->algorithm;
}

VN_CRYPTO_NS_END
