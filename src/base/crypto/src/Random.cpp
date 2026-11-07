#include <vine/crypto/Random.hpp>

#include <mutex>
#include <stdexcept>

#include <wolfssl/wolfcrypt/random.h>

#include "WolfSsl.hpp"

VN_CRYPTO_NS_BEGIN

namespace
{

/**
 * @brief Fills a buffer from the process-wide DRBG, initializing it on first use.
 *
 * @details
 * - One context is enough for this module: the DRBG reseeds itself, and serialising the calls costs less than one
 *   context per thread at the sizes this API is used at (keys, nonces, salts).
 * - The lock is held across initialization *and* generation, because a `WC_RNG` is not safe to use concurrently and a
 *   failure must be retried on the next call rather than cached as a permanently broken state.
 * - The generator answers in 32-bit counts, so a large request is fed in chunks.
 *
 * @param out  First byte to fill.
 * @param size Number of bytes to fill; must be non-zero.
 * @throws std::runtime_error when the context cannot be initialized or the source fails.
 */
void generateBlock(unsigned char* out, std::size_t size)
{
    static WC_RNG     rng{};
    static bool       ready = false;
    static std::mutex guard;

    const std::lock_guard<std::mutex> lock(guard);

    // The RNG locks a state mutex that wolfSSL only initializes in wolfCrypt_Init(); skipping it faults on first use.
    detail::ensureLibraryInitialized();

    if (!ready) {
        if (wc_InitRng(&rng) != 0) {
            throw std::runtime_error("vn::crypto::Random: the random source could not be initialized");
        }
        ready = true;
    }

    while (size > 0) {
        const auto chunk = size > 0xFFFFFFFFu ? static_cast<std::size_t>(0xFFFFFFFFu) : size;
        if (wc_RNG_GenerateBlock(&rng, out, static_cast<word32>(chunk)) != 0) {
            throw std::runtime_error("vn::crypto::Random: the random source failed");
        }
        out += chunk;
        size -= chunk;
    }
}

} // namespace

void Random::fill(std::span<std::byte> buffer)
{
    if (buffer.empty()) {
        return;
    }

    fill(buffer.data(), buffer.size());
}

void Random::fill(void* data, std::size_t size)
{
    if (size == 0) {
        return;
    }
    if (data == nullptr) {
        throw std::invalid_argument("vn::crypto::Random::fill: null buffer with a non-zero size");
    }

    generateBlock(static_cast<unsigned char*>(data), size);
}

std::vector<std::byte> Random::bytes(std::size_t size)
{
    std::vector<std::byte> out(size);
    fill(out);
    return out;
}

VN_CRYPTO_NS_END
