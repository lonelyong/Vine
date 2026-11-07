vn_add_library(CRYPTO_TARGET_NAME Crypto)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

if(VN_USE_FETCHCONTENT)
    # libsodium is a pure autotools project (no CMakeLists.txt), so it cannot be
    # consumed via FetchContent. wolfSSL ships a CMake build, exposes a single
    # wolfssl target and can be trimmed to crypto-only (hashes, AES, ChaCha20,
    # HMAC) via WOLFSSL_CRYPT_ONLY.
    FetchContent_Declare(wolfssl
        GIT_REPOSITORY https://github.com/wolfSSL/wolfssl.git
        # Pinned on purpose: a crypto backend must not move under the build silently.
        # 5.9.4 (2026-09-25) fixes the CVE set that 5.9.2 and earlier carry; bumping again means
        # re-reading its release notes for API/behaviour changes, then rebuilding + re-running test_crypto.
        GIT_TAG        v5.9.4-stable
    )
    # Crypto-only build: no TLS stack, no examples, no tests, no install rules.
    set(WOLFSSL_CRYPT_ONLY yes CACHE STRING "" FORCE)
    # MD5 is on by default in this build (digest `Digest::kMd5`); NO_MD5 is not set anywhere.
    #
    # The knobs below are wolfSSL's own option names, and they matter: the CMake option
    # appends the *macro* to the library's compile definitions, which is the only thing
    # rsa.c / aes.c actually see. Naming them after the macro (WOLFSSL_KEY_GEN) silently
    # does nothing but fill options.h, and consumers then compile code the library lacks.
    # Key generation (macro WOLFSSL_KEY_GEN): needed for RSA / ECC keys.
    set(WOLFSSL_KEYGEN yes CACHE STRING "" FORCE)
    # AES-CTR (macros WOLFSSL_AES_COUNTER / WOLFSSL_AES_DIRECT).
    set(WOLFSSL_AESCTR yes CACHE STRING "" FORCE)
    # MD5 (`Digest::kMd5`, legacy interop only). wolfSSL's default for this knob is *disabled* and only some preset
    # turns it back on, so leaving it implicit makes the module's own `Digest` enum depend on a side effect: a tree
    # configured without that preset builds `md5.h` empty and `DigestOps.cpp` stops on `wc_Md5`.
    set(WOLFSSL_MD5 yes CACHE STRING "" FORCE)
    # Argon2 (RFC 9106) for password hashing. It is off by default and pulls BLAKE2b in with it.
    set(WOLFSSL_ARGON2 yes CACHE STRING "" FORCE)
    # Encrypted PEM keys (macro WOLFSSL_ENCRYPTED_KEYS): the legacy `Proc-Type: 4,ENCRYPTED` / `DEK-Info:` form that
    # `openssl rsa -aes128` wrote before PKCS#8 took over. Without it `wc_KeyPemToDer` drops the password on the floor
    # and returns bytes that are still encrypted, so the failure looks like a bad key rather than a missing feature.
    set(WOLFSSL_ENCKEYS yes CACHE STRING "" FORCE)
    # System CA certificates are a TLS feature: with CRYPT_ONLY, asn.c still references the
    # GetCA/GetCAByName that only the TLS layer defines, so the link fails.
    set(WOLFSSL_SYS_CA_CERTS no CACHE BOOL "" FORCE)
    # MSVC has neither <unistd.h> nor pid_t, yet wolfSSL's `check_symbol_exists(getpid ...)` can answer
    # yes on this host (a toolchain that only links a static library reports every symbol as found).
    # random.h then declares `pid_t pid;` inside WC_RNG and the build stops. Seeding the cache keeps the
    # probe from running, so neither options.h nor the library's own definitions claim HAVE_GETPID.
    set(HAVE_GETPID OFF CACHE BOOL "" FORCE)
    set(WOLFSSL_EXAMPLES no CACHE STRING "" FORCE)
    set(WOLFSSL_CRYPT_TESTS no CACHE STRING "" FORCE)
    set(WOLFSSL_INSTALL no CACHE STRING "" FORCE)
    # RSA / ECC need a big-integer backend. With this off, rsa.c and ecc.c still reference
    # sp_init / sp_init_size and the link fails; single-precision math is also the fastest option here.
    set(WOLFSSL_SP_MATH_ALL yes CACHE STRING "" FORCE)
    # Bake a static wolfssl into the crypto DLL.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(wolfssl)

    # The project compiles C as C17 (CMAKE_C_STANDARD), which makes wolfSSL pick
    # its C99 variable-length-array stack path (WOLFSSL_SP_DYN_STACK). MSVC does
    # not support VLAs, so force fixed-size stack buffers instead.
    target_compile_definitions(wolfssl PRIVATE WOLFSSL_SP_NO_DYN_STACK)

    # WOLFSSL_CRYPT_ONLY trims the source list, but it does not define the macro the
    # sources are guarded by: without WOLFCRYPT_ONLY, asn.c still compiles the TLS
    # certificate paths that call GetCA/GetCAByName, whose definitions live in the
    # excluded TLS sources, and the link fails.
    # PUBLIC on purpose: WOLFCRYPT_ONLY also changes the layout of key structures such
    # as RsaKey, so every consumer must see the same value or the backend writes at
    # offsets the caller never allocated (symptoms: sign succeeds, verify always fails).
    target_compile_definitions(wolfssl PUBLIC WOLFCRYPT_ONLY)

    # Streaming AES-GCM (wc_AesGcmEncryptInit/Update/Final) adds state to the Aes context. The library needs the
    # macro here; consumers cannot inherit it, because the generated options.h emits `#undef WOLFSSL_AESGCM_STREAM`
    # for every unit that includes it (see src/WolfSsl.hpp, which re-asserts it).
    target_compile_definitions(wolfssl PRIVATE WOLFSSL_AESGCM_STREAM)

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

# Sources (GLOB): AesCbc.cpp, AesCtr.cpp, AesGcm.cpp, Argon2.cpp, ByteSequenceFingerprint.cpp, Digest.cpp,
# DigestOps.cpp, EccKey.cpp, Hasher.cpp, Hkdf.cpp, Hmac.cpp, Pbkdf2.cpp, Pem.cpp, Pkcs8.cpp, Random.cpp, RsaKey.cpp,
# WolfSsl.cpp
