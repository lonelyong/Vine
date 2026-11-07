#include <vine/crypto/Digest.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

#include "DigestOps.hpp"

VN_CRYPTO_NS_BEGIN

namespace
{

/// Sizes in bytes, in the order of the `Digest` enumerators.
constexpr std::size_t kSizes[] = { 16, 20, 28, 32, 48, 64, 28, 32, 48, 64 };

/// Canonical names, in the order of the `Digest` enumerators.
constexpr std::u8string_view kNames[] = { u8"md5",     u8"sha1",     u8"sha224",   u8"sha256",   u8"sha384",
                                          u8"sha512",  u8"sha3_224", u8"sha3_256", u8"sha3_384", u8"sha3_512" };

/**
 * @brief Returns the enumerator's index, which indexes the tables above.
 *
 * @param digest The digest.
 * @return The zero-based index.
 */
std::size_t indexOf(Digest digest)
{
    return static_cast<std::size_t>(digest);
}

/**
 * @brief Normalizes a digest name so that separators and case do not matter.
 *
 * @param name The caller's spelling.
 * @return The name in lower case with every `-`, `_` and space removed.
 */
std::string normalizeName(std::u8string_view name)
{
    std::string out;
    out.reserve(name.size());
    for (const auto ch : name) {
        const auto raw = static_cast<unsigned char>(ch);
        if (std::isalnum(raw) != 0) {
            out.push_back(static_cast<char>(std::tolower(raw)));
        }
    }
    return out;
}

} // namespace

std::size_t digestSize(Digest digest)
{
    return kSizes[indexOf(digest)];
}

std::u8string_view digestName(Digest digest)
{
    return kNames[indexOf(digest)];
}

Digest digestFromName(std::u8string_view name)
{
    const auto wanted = normalizeName(name);
    for (std::size_t index = 0; index < std::size(kNames); ++index) {
        if (wanted == normalizeName(kNames[index])) {
            return static_cast<Digest>(index);
        }
    }

    throw std::invalid_argument("vn::crypto::digestFromName: unknown digest name");
}

std::vector<std::byte> digest(Digest algorithm, std::span<const std::byte> data)
{
    detail::DigestContext context;
    if (!detail::digestInit(algorithm, context)) {
        throw std::runtime_error("vn::crypto::digest: the backend could not initialize the digest");
    }

    if (!detail::digestUpdate(algorithm, context, data.data(), data.size())) {
        detail::digestFree(algorithm, context);
        throw std::runtime_error("vn::crypto::digest: the backend rejected the input");
    }

    std::vector<std::byte> out(digestSize(algorithm));
    if (!detail::digestFinal(algorithm, context, reinterpret_cast<unsigned char*>(out.data()))) {
        throw std::runtime_error("vn::crypto::digest: the backend could not finalize the digest");
    }

    return out;
}

VN_CRYPTO_NS_END
