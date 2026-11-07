#include "WolfSsl.hpp"

#include <mutex>
#include <stdexcept>

#include <wolfssl/wolfcrypt/wc_port.h>

VN_CRYPTO_NS_BEGIN

namespace detail
{

void ensureLibraryInitialized()
{
    static std::once_flag once;

    // std::call_once rethrows to every waiting caller and clears the flag, so a failure is not cached.
    std::call_once(once, [] {
        if (wolfCrypt_Init() != 0) {
            throw std::runtime_error("vn::crypto: the crypto backend could not be initialized");
        }
    });
}

RandomHandle::RandomHandle()
{
    ensureLibraryInitialized();
    if (wc_InitRng(&context_) != 0) {
        throw std::runtime_error("vn::crypto: a random context could not be initialized");
    }
    ready_ = true;
}

RandomHandle::~RandomHandle()
{
    if (ready_) {
        wc_FreeRng(&context_);
    }
}

WC_RNG* RandomHandle::get()
{
    return &context_;
}

} // namespace detail

VN_CRYPTO_NS_END
