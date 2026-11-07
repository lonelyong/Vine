#pragma once

#include <vine/crypto/crypto_global.hpp>

#include <cstddef>

// settings.h pulls in the generated options.h. That file unconditionally emits `#undef` for every option it knows,
// including `WOLFSSL_AESGCM_STREAM`, so the define cannot come from the command line for any translation unit that
// includes it. The backend library is compiled with the macro (target_compile_definitions on the wolfssl target) and
// the `Aes` context grows extra streaming state when it is present, so both sides must agree: re-assert it here, after
// settings.h has run, and include aes.h from this header so no unit can get there with the wrong layout.
#include <wolfssl/wolfcrypt/settings.h>

#ifndef WOLFSSL_AESGCM_STREAM
    #define WOLFSSL_AESGCM_STREAM
#endif

#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/random.h>

VN_CRYPTO_NS_BEGIN

namespace detail
{

/**
 * @brief Runs wolfSSL's process-wide initialization exactly once.
 *
 * @details
 * - Every wolfSSL entry point that touches global state (the RNG's state mutex, the ASN.1/DRBG tables) expects
 *   `wolfCrypt_Init()` to have run; calling the RNG without it locks an uninitialized mutex and faults.
 * - The call is idempotent and thread-safe: it succeeds once for the process, and a failed attempt is retried by the
 *   next caller rather than latched as a permanently broken state.
 * - Cleanup is deliberately not registered: `wolfCrypt_Cleanup()` would run from a static destructor, after other
 *   globals may already be gone. The process ends, and with it the state.
 *
 * @throws std::runtime_error when wolfSSL reports that it could not initialize.
 */
void ensureLibraryInitialized();

/**
 * @brief Owns one backend random context for the duration of a single operation.
 *
 * @details
 * - Key generation and randomized signing need an RNG. One context per operation keeps the key objects free of shared
 *   mutable state, so no lock is needed around the backend calls and two threads may use different keys at once.
 * - Initializing a context seeds it, which is not free: create one per operation, never per byte.
 *
 * @warning
 * - The handle is move-only by construction (copying is deleted), and the context is released in the destructor.
 */
class RandomHandle
{
  public:
    /**
     * @brief Seeds a fresh backend random context.
     *
     * @throws std::runtime_error when the backend cannot provide a context.
     */
    RandomHandle();

    ~RandomHandle();

    RandomHandle(const RandomHandle&)            = delete;
    RandomHandle& operator=(const RandomHandle&) = delete;

    /**
     * @brief Returns the context for the backend call.
     *
     * @return The initialized context; valid until this handle is destroyed.
     */
    WC_RNG* get();

  private:
    WC_RNG context_{};
    bool   ready_{ false };
};

} // namespace detail

VN_CRYPTO_NS_END
