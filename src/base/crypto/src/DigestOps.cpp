#include "DigestOps.hpp"

#include <cstring>

#include <wolfssl/wolfcrypt/md5.h>
#include <wolfssl/wolfcrypt/sha.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/sha3.h>
#include <wolfssl/wolfcrypt/sha512.h>

VN_CRYPTO_NS_BEGIN

namespace detail
{

namespace
{

static_assert(sizeof(wc_Md5) <= kDigestContextSize, "the digest context storage is too small for MD5");
static_assert(sizeof(wc_Sha) <= kDigestContextSize, "the digest context storage is too small for SHA-1");
static_assert(sizeof(wc_Sha256) <= kDigestContextSize, "the digest context storage is too small for SHA-256");
static_assert(sizeof(wc_Sha512) <= kDigestContextSize, "the digest context storage is too small for SHA-512");
static_assert(sizeof(wc_Sha3) <= kDigestContextSize, "the digest context storage is too small for SHA-3");

/**
 * @brief Views the opaque storage as one backend context type.
 *
 * @tparam Context The backend context type.
 * @param context The storage.
 * @return A pointer to the context inside the storage.
 */
template <typename Context>
Context* as(DigestContext& context)
{
    return reinterpret_cast<Context*>(context.storage.data());
}

/**
 * @brief Reads the opaque storage as one backend context type.
 *
 * @tparam Context The backend context type.
 * @param context The storage.
 * @return A pointer to the context inside the storage.
 */
template <typename Context>
const Context* as(const DigestContext& context)
{
    return reinterpret_cast<const Context*>(context.storage.data());
}

} // namespace

enum wc_HashType wolfHashType(Digest digest)
{
    switch (digest) {
    case Digest::kMd5: return WC_HASH_TYPE_MD5;
    case Digest::kSha1: return WC_HASH_TYPE_SHA;
    case Digest::kSha224: return WC_HASH_TYPE_SHA224;
    case Digest::kSha256: return WC_HASH_TYPE_SHA256;
    case Digest::kSha384: return WC_HASH_TYPE_SHA384;
    case Digest::kSha512: return WC_HASH_TYPE_SHA512;
    case Digest::kSha3_224: return WC_HASH_TYPE_SHA3_224;
    case Digest::kSha3_256: return WC_HASH_TYPE_SHA3_256;
    case Digest::kSha3_384: return WC_HASH_TYPE_SHA3_384;
    case Digest::kSha3_512: return WC_HASH_TYPE_SHA3_512;
    }
    return WC_HASH_TYPE_SHA256;
}

bool digestInit(Digest digest, DigestContext& context)
{
    switch (digest) {
    case Digest::kMd5: return wc_InitMd5(as<wc_Md5>(context)) == 0;
    case Digest::kSha1: return wc_InitSha(as<wc_Sha>(context)) == 0;
    case Digest::kSha224: return wc_InitSha224(as<wc_Sha224>(context)) == 0;
    case Digest::kSha256: return wc_InitSha256(as<wc_Sha256>(context)) == 0;
    case Digest::kSha384: return wc_InitSha384(as<wc_Sha384>(context)) == 0;
    case Digest::kSha512: return wc_InitSha512(as<wc_Sha512>(context)) == 0;
    // SHA-3 init also takes a heap pointer and a device id; both are unused in a software-only build.
    case Digest::kSha3_224: return wc_InitSha3_224(as<wc_Sha3>(context), nullptr, 0) == 0;
    case Digest::kSha3_256: return wc_InitSha3_256(as<wc_Sha3>(context), nullptr, 0) == 0;
    case Digest::kSha3_384: return wc_InitSha3_384(as<wc_Sha3>(context), nullptr, 0) == 0;
    case Digest::kSha3_512: return wc_InitSha3_512(as<wc_Sha3>(context), nullptr, 0) == 0;
    }
    return false;
}

bool digestUpdate(Digest digest, DigestContext& context, const void* data, std::size_t size)
{
    if (size == 0) {
        return true;
    }

    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t left  = size;

    // Every backend update function takes a 32-bit length, so feed the input in chunks.
    while (left > 0) {
        const auto  chunk = left > 0xFFFFFFFFu ? static_cast<std::size_t>(0xFFFFFFFFu) : left;
        const auto* input = bytes;
        const auto  length = static_cast<word32>(chunk);
        int         status = 0;

        switch (digest) {
        case Digest::kMd5: status = wc_Md5Update(as<wc_Md5>(context), input, length); break;
        case Digest::kSha1: status = wc_ShaUpdate(as<wc_Sha>(context), input, length); break;
        case Digest::kSha224: status = wc_Sha224Update(as<wc_Sha224>(context), input, length); break;
        case Digest::kSha256: status = wc_Sha256Update(as<wc_Sha256>(context), input, length); break;
        case Digest::kSha384: status = wc_Sha384Update(as<wc_Sha384>(context), input, length); break;
        case Digest::kSha512: status = wc_Sha512Update(as<wc_Sha512>(context), input, length); break;
        case Digest::kSha3_224: status = wc_Sha3_224_Update(as<wc_Sha3>(context), input, length); break;
        case Digest::kSha3_256: status = wc_Sha3_256_Update(as<wc_Sha3>(context), input, length); break;
        case Digest::kSha3_384: status = wc_Sha3_384_Update(as<wc_Sha3>(context), input, length); break;
        case Digest::kSha3_512: status = wc_Sha3_512_Update(as<wc_Sha3>(context), input, length); break;
        }

        if (status != 0) {
            return false;
        }

        bytes += chunk;
        left -= chunk;
    }

    return true;
}

bool digestFinal(Digest digest, DigestContext& context, unsigned char* out)
{
    int status = 0;

    switch (digest) {
    case Digest::kMd5:
        status = wc_Md5Final(as<wc_Md5>(context), out);
        wc_Md5Free(as<wc_Md5>(context));
        break;
    case Digest::kSha1:
        status = wc_ShaFinal(as<wc_Sha>(context), out);
        wc_ShaFree(as<wc_Sha>(context));
        break;
    case Digest::kSha224:
        status = wc_Sha224Final(as<wc_Sha224>(context), out);
        wc_Sha224Free(as<wc_Sha224>(context));
        break;
    case Digest::kSha256:
        status = wc_Sha256Final(as<wc_Sha256>(context), out);
        wc_Sha256Free(as<wc_Sha256>(context));
        break;
    case Digest::kSha384:
        status = wc_Sha384Final(as<wc_Sha384>(context), out);
        wc_Sha384Free(as<wc_Sha384>(context));
        break;
    case Digest::kSha512:
        status = wc_Sha512Final(as<wc_Sha512>(context), out);
        wc_Sha512Free(as<wc_Sha512>(context));
        break;
    case Digest::kSha3_224:
        status = wc_Sha3_224_Final(as<wc_Sha3>(context), out);
        wc_Sha3_224_Free(as<wc_Sha3>(context));
        break;
    case Digest::kSha3_256:
        status = wc_Sha3_256_Final(as<wc_Sha3>(context), out);
        wc_Sha3_256_Free(as<wc_Sha3>(context));
        break;
    case Digest::kSha3_384:
        status = wc_Sha3_384_Final(as<wc_Sha3>(context), out);
        wc_Sha3_384_Free(as<wc_Sha3>(context));
        break;
    case Digest::kSha3_512:
        status = wc_Sha3_512_Final(as<wc_Sha3>(context), out);
        wc_Sha3_512_Free(as<wc_Sha3>(context));
        break;
    }

    return status == 0;
}

void digestFree(Digest digest, DigestContext& context)
{
    switch (digest) {
    case Digest::kMd5: wc_Md5Free(as<wc_Md5>(context)); break;
    case Digest::kSha1: wc_ShaFree(as<wc_Sha>(context)); break;
    case Digest::kSha224: wc_Sha224Free(as<wc_Sha224>(context)); break;
    case Digest::kSha256: wc_Sha256Free(as<wc_Sha256>(context)); break;
    case Digest::kSha384: wc_Sha384Free(as<wc_Sha384>(context)); break;
    case Digest::kSha512: wc_Sha512Free(as<wc_Sha512>(context)); break;
    case Digest::kSha3_224: wc_Sha3_224_Free(as<wc_Sha3>(context)); break;
    case Digest::kSha3_256: wc_Sha3_256_Free(as<wc_Sha3>(context)); break;
    case Digest::kSha3_384: wc_Sha3_384_Free(as<wc_Sha3>(context)); break;
    case Digest::kSha3_512: wc_Sha3_512_Free(as<wc_Sha3>(context)); break;
    }
}

} // namespace detail

VN_CRYPTO_NS_END
