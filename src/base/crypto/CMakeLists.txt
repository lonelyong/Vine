vn_add_library(CRYPTO_TARGET_NAME Crypto)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

if(VINE_USE_FETCHCONTENT)
    # libsodium is a pure autotools project (no CMakeLists.txt), so it cannot be
    # consumed via FetchContent. wolfSSL ships a CMake build, exposes a single
    # wolfssl target and can be trimmed to crypto-only (hashes, AES, ChaCha20,
    # HMAC) via WOLFSSL_CRYPT_ONLY.
    FetchContent_Declare(wolfssl
        GIT_REPOSITORY https://github.com/wolfSSL/wolfssl.git
        GIT_TAG        v5.9.2-stable
    )
    # Crypto-only build: no TLS stack, no examples, no tests, no install rules.
    set(WOLFSSL_CRYPT_ONLY yes CACHE STRING "" FORCE)
    # MD5 is disabled by default in wolfSSL; enable it for the Hash API.
    set(WOLFSSL_MD5 yes CACHE STRING "" FORCE)
    set(WOLFSSL_EXAMPLES no CACHE STRING "" FORCE)
    set(WOLFSSL_CRYPT_TESTS no CACHE STRING "" FORCE)
    set(WOLFSSL_INSTALL no CACHE STRING "" FORCE)
    # The crypto scope (hashes, ciphers, HMAC) does not need big-integer math,
    # so drop the single-precision math implementation to shrink the library.
    set(WOLFSSL_SP_MATH_ALL no CACHE STRING "" FORCE)
    # Bake a static wolfssl into the crypto DLL.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(wolfssl)

    # The project compiles C as C17 (CMAKE_C_STANDARD), which makes wolfSSL pick
    # its C99 variable-length-array stack path (WOLFSSL_SP_DYN_STACK). MSVC does
    # not support VLAs, so force fixed-size stack buffers instead.
    target_compile_definitions(wolfssl PRIVATE WOLFSSL_SP_NO_DYN_STACK)

    # Consumers must see the wolfSSL build options (SHA-224/384/512, MD5, ...) so
    # the enabled-feature guards in the wolfcrypt headers match the library build.
    target_compile_definitions(${CRYPTO_TARGET_NAME} PRIVATE WOLFSSL_USE_OPTIONS_H)

    # Group every third-party target under the third_party solution folder.
    vn_group_targets_folder(${wolfssl_SOURCE_DIR} third_party)
else()
    # Consume wolfSSL from vcpkg or a local install.
    find_package(wolfssl CONFIG REQUIRED)
    if(NOT TARGET wolfssl AND TARGET wolfssl::wolfssl)
        add_library(wolfssl ALIAS wolfssl::wolfssl)
    endif()
endif()

target_link_libraries(${CRYPTO_TARGET_NAME} PUBLIC vn::Global vn::Core)
# Static wolfssl is baked into the crypto DLL.
target_link_libraries(${CRYPTO_TARGET_NAME} PRIVATE wolfssl)

# Sources (GLOB): Hash.cpp, HashCalculator.cpp
