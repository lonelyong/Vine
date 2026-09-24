#include <vine/crypto/Hash.hpp>
#include <vine/MemoryStream.hpp>

#include <array>
#include <span>
#include <vector>

#include <wolfssl/wolfcrypt/md5.h>
#include <wolfssl/wolfcrypt/sha.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/sha3.h>
#include <wolfssl/wolfcrypt/sha512.h>

VN_CRYPTO_NS_BEGIN

namespace
{

/**
 * @brief Computes a digest with the streaming wolfSSL API.
 *
 * @tparam HashT wolfSSL hash context type.
 * @tparam Init Update Final Free Streaming functions.
 * @tparam Size Digest size in bytes.
 */
template <typename HashT, int (*Init)(HashT*), int (*Update)(HashT*, const byte*, word32),
          int (*Final)(HashT*, byte*), void (*Free)(HashT*), std::size_t Size>
std::array<std::uint8_t, Size> digest(const void* data, std::size_t size)
{
    std::array<std::uint8_t, Size> out{};
    HashT                          hash;
    if (Init(&hash) != 0) {
        return out;
    }
    // The update functions take a 32-bit length, so feed the input in chunks.
    const auto* p = static_cast<const unsigned char*>(data);
    while (size > 0) {
        const word32 chunk = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<word32>(size);
        if (Update(&hash, p, chunk) != 0) {
            Free(&hash);
            return out;
        }
        p += chunk;
        size -= chunk;
    }
    const int ret = Final(&hash, out.data());
    Free(&hash);
    if (ret != 0) {
        out.fill(0);
    }
    return out;
}

/**
 * @brief Adapts the three-argument SHA-3 init to the one-argument digest signature.
 *
 * SHA-3 init also takes a heap pointer and a device id; both are unused in the
 * software-only build, so the digest template can treat them as a single call.
 */
int initSha3_224(wc_Sha3* hash)
{
    return wc_InitSha3_224(hash, nullptr, 0);
}

int initSha3_256(wc_Sha3* hash)
{
    return wc_InitSha3_256(hash, nullptr, 0);
}

int initSha3_384(wc_Sha3* hash)
{
    return wc_InitSha3_384(hash, nullptr, 0);
}

int initSha3_512(wc_Sha3* hash)
{
    return wc_InitSha3_512(hash, nullptr, 0);
}

/**
 * @brief Reads all remaining bytes of a stream.
 *
 * The bytes are read through the stream buffer's get area, so a MemoryStream
 * (which places its wrapped bytes there) yields its whole content.
 *
 * @param in The stream to read from.
 * @return The bytes read.
 */
std::vector<unsigned char> readAll(std::ostream& in)
{
    std::vector<unsigned char> data;
    if (auto* buf = in.rdbuf()) {
        char            chunk[4096];
        std::streamsize n;
        while ((n = buf->sgetn(chunk, sizeof chunk)) > 0) {
            data.insert(data.end(), chunk, chunk + n);
        }
    }
    return data;
}

/**
 * @brief Wraps a byte range as a readable memory stream.
 *
 * @param data Pointer to the first byte, or nullptr for an empty range.
 * @param size Number of bytes; ignored when @p data is nullptr.
 * @return A stream holding a copy of the range.
 */
MemoryStream wrapBytes(const void* data, std::size_t size)
{
    if (data == nullptr || size == 0) {
        return MemoryStream();
    }
    return MemoryStream(std::span<const std::byte>(static_cast<const std::byte*>(data), size));
}

} // namespace

std::array<std::uint8_t, 32> Hash::sha256(const void* data, std::size_t size)
{
    MemoryStream stream = wrapBytes(data, size);
    return sha256(stream);
}

std::array<std::uint8_t, 32> Hash::sha256(std::ostream& in)
{
    const auto bytes = readAll(in);
    return digest<wc_Sha256, wc_InitSha256, wc_Sha256Update, wc_Sha256Final, wc_Sha256Free, 32>(bytes.data(), bytes.size());
}

std::array<std::uint8_t, 16> Hash::md5(const void* data, std::size_t size)
{
    MemoryStream stream = wrapBytes(data, size);
    return md5(stream);
}

std::array<std::uint8_t, 16> Hash::md5(std::ostream& in)
{
    const auto bytes = readAll(in);
    return digest<wc_Md5, wc_InitMd5, wc_Md5Update, wc_Md5Final, wc_Md5Free, 16>(bytes.data(), bytes.size());
}

std::array<std::uint8_t, 20> Hash::sha1(const void* data, std::size_t size)
{
    MemoryStream stream = wrapBytes(data, size);
    return sha1(stream);
}

std::array<std::uint8_t, 20> Hash::sha1(std::ostream& in)
{
    const auto bytes = readAll(in);
    return digest<wc_Sha, wc_InitSha, wc_ShaUpdate, wc_ShaFinal, wc_ShaFree, 20>(bytes.data(), bytes.size());
}

std::array<std::uint8_t, 28> Hash::sha224(const void* data, std::size_t size)
{
    MemoryStream stream = wrapBytes(data, size);
    return sha224(stream);
}

std::array<std::uint8_t, 28> Hash::sha224(std::ostream& in)
{
    const auto bytes = readAll(in);
    return digest<wc_Sha224, wc_InitSha224, wc_Sha224Update, wc_Sha224Final, wc_Sha256Free, 28>(bytes.data(), bytes.size());
}

std::array<std::uint8_t, 48> Hash::sha384(const void* data, std::size_t size)
{
    MemoryStream stream = wrapBytes(data, size);
    return sha384(stream);
}

std::array<std::uint8_t, 48> Hash::sha384(std::ostream& in)
{
    const auto bytes = readAll(in);
    return digest<wc_Sha384, wc_InitSha384, wc_Sha384Update, wc_Sha384Final, wc_Sha384Free, 48>(bytes.data(), bytes.size());
}

std::array<std::uint8_t, 64> Hash::sha512(const void* data, std::size_t size)
{
    MemoryStream stream = wrapBytes(data, size);
    return sha512(stream);
}

std::array<std::uint8_t, 64> Hash::sha512(std::ostream& in)
{
    const auto bytes = readAll(in);
    return digest<wc_Sha512, wc_InitSha512, wc_Sha512Update, wc_Sha512Final, wc_Sha512Free, 64>(bytes.data(), bytes.size());
}

std::array<std::uint8_t, 28> Hash::sha3_224(const void* data, std::size_t size)
{
    MemoryStream stream = wrapBytes(data, size);
    return sha3_224(stream);
}

std::array<std::uint8_t, 28> Hash::sha3_224(std::ostream& in)
{
    const auto bytes = readAll(in);
    return digest<wc_Sha3, initSha3_224, wc_Sha3_224_Update, wc_Sha3_224_Final, wc_Sha3_224_Free, 28>(bytes.data(), bytes.size());
}

std::array<std::uint8_t, 32> Hash::sha3_256(const void* data, std::size_t size)
{
    MemoryStream stream = wrapBytes(data, size);
    return sha3_256(stream);
}

std::array<std::uint8_t, 32> Hash::sha3_256(std::ostream& in)
{
    const auto bytes = readAll(in);
    return digest<wc_Sha3, initSha3_256, wc_Sha3_256_Update, wc_Sha3_256_Final, wc_Sha3_256_Free, 32>(bytes.data(), bytes.size());
}

std::array<std::uint8_t, 48> Hash::sha3_384(const void* data, std::size_t size)
{
    MemoryStream stream = wrapBytes(data, size);
    return sha3_384(stream);
}

std::array<std::uint8_t, 48> Hash::sha3_384(std::ostream& in)
{
    const auto bytes = readAll(in);
    return digest<wc_Sha3, initSha3_384, wc_Sha3_384_Update, wc_Sha3_384_Final, wc_Sha3_384_Free, 48>(bytes.data(), bytes.size());
}

std::array<std::uint8_t, 64> Hash::sha3_512(const void* data, std::size_t size)
{
    MemoryStream stream = wrapBytes(data, size);
    return sha3_512(stream);
}

std::array<std::uint8_t, 64> Hash::sha3_512(std::ostream& in)
{
    const auto bytes = readAll(in);
    return digest<wc_Sha3, initSha3_512, wc_Sha3_512_Update, wc_Sha3_512_Final, wc_Sha3_512_Free, 64>(bytes.data(), bytes.size());
}

VN_CRYPTO_NS_END
