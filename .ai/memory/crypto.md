# crypto 要点

- 后端是 wolfSSL **v5.9.4-stable**（`src/base/crypto/CMakeLists.txt` 固定 tag），静态煮进 `VineCrypto`；
  `WOLFSSL_USE_OPTIONS_H` 让头文件与库看到同一份 `options.h`。
- 后端全局初始化只在 `src/base/crypto/src/WolfSsl.cpp` 的 `detail::ensureLibraryInitialized()`（`std::call_once`）。
  跳过它会崩：`wc_InitRng` 会锁只由 `wolfCrypt_Init()` 初始化的 `drbgStateMutex`。
- wolfSSL 头文件把 `Md5` / `Sha256` / `Sha384` 等定义成宏 → 自己的枚举值一律 `k` 前缀（`Digest::kSha256`）。
- `vn::crypto::Hmac` 与 wolfSSL 的 `::Hmac` 同名：PImpl 里的后端上下文必须写 `::Hmac`。
- 摘要只有一套词汇：`Digest` 枚举 + `digest(Digest, span)` 一次性 + `Hasher` 增量；
  `Hmac` / `RsaKey` / `EccKey` 都收 `Digest`。`Hash` / `HashCalculator` / `Hmac::Algorithm` 已删除（ADR-2026-11-07-digest-vocabulary）。
- 对称：`AesGcm`（AEAD，默认）、`AesCbc`（PKCS#7）、`AesCtr`（无填充）——后两者只为互通，都不认证。
- 口令哈希：`Argon2`（推荐，需 CMake 选项 `WOLFSSL_ARGON2`，会带进 BLAKE2b）与 `Pbkdf2`（互通）。
  测试向量：PBKDF2 = RFC 6070 四个向量；AES-CBC = NIST SP 800-38A F.2.1；AES-CTR = F.5.1。
- 口令保护的私钥：`RsaKey` / `EccKey` 的 `toEncryptedPrivateDer/Pem` 与 `fromEncryptedPrivateDer/Pem`，
  实现只在 `src/Pkcs8.cpp`（`detail::encryptPkcs8Der` / `decryptPkcs8Der`）。
  wolfSSL 入口：`wc_CreateEncryptedPKCS8Key(..., PKCS5, PBES2, AES256CBCb, salt=nullptr, 0, itt, rng, nullptr)` 写出，
  `wc_DecryptPKCS8Key` + `wc_GetPkcs8TraditionalOffset` 读回；三者都在 `HAVE_PKCS8` 下（`NO_PKCS8` 未定义即开启，本构建已够）。
  `AES256CBCb` / `PBES2` / `PKCS5` 来自 `wolfssl/wolfcrypt/asn.h`（经 `types.h` → `oid_sum.h`），不是自造常量。
- `AesGcm` 的增量形态缓冲全部输入（`P4`）；密文长度 == 明文长度，标签 16 字节另行存放。
- `AesGcm::finish()` 在解密侧先验标签再返回明文，失败抛 `std::runtime_error` 并丢弃缓冲。
- `Random` 是进程级 DRBG + 一把互斥锁，按 32 位分块；`size == 0` 是空操作。
- 测试向量：HMAC = RFC 4231 case 2；AES-GCM = NIST SP 800-38D case 2；HKDF = RFC 5869 test case 1。
- 升级 wolfSSL 后必查：`HAVE_GETPID` 在 MSVC 上会被误判（不然 `WC_RNG` 里的 `pid_t` 编译不过），
  CMake 预置 `set(HAVE_GETPID OFF CACHE BOOL "" FORCE)` 必须在 `FetchContent_MakeAvailable` 之前。
- 恒定时间比较用后端的 `wc_ConstantCompare()`（`wolfssl/wolfcrypt/memory.h`，5.9.4 起公开），不再自己写循环。
- **options.h 会 `#undef` 它认识的每个宏**，所以命令行/`target_compile_definitions` 给消费者加的宏活不过 settings.h；
  需要消费者也看到的宏（如 `WOLFSSL_AESGCM_STREAM`）在 `src/WolfSsl.hpp` 里“先 include settings.h，再 `#define`，再 include aes.h”。
- AES-GCM 是流式（`wc_AesGcm{Encrypt,Decrypt}{Init,Update,Final}`，需 `WOLFSSL_AESGCM_STREAM`）；解密输出在 `finish()` 前不可信。
- HKDF 在 `wc_HKDF` / `wc_HKDF_Extract` / `wc_HKDF_Expand`（wolfssl/wolfcrypt/hmac.h）；输出上限 `255 × digestSize`。
- RSA 公钥导出用 `wc_RsaPublicKeyDerSize(key, 1)` + `wc_RsaKeyToPublicDer()`（后者在 `asn_public.h`，不在 `rsa.h`）。
- PEM：编码用 `wc_DerToPem(der, sz, out, outSz, type)`（type 是 `CertType` 枚举：`PRIVATEKEY_TYPE`/`PUBLICKEY_TYPE`/
  `ECC_PRIVATEKEY_TYPE`）；解码自己做（`src/Pem.cpp`），因为 `wc_KeyPemToDer` 写死了解私钥、解不了 `PUBLIC KEY`。
- **wolfSSL 的构建旋钮**：CMake 选项名与宏名不同，写错会静默失效——`WOLFSSL_KEYGEN`（宏 `WOLFSSL_KEY_GEN`）、
  `WOLFSSL_AESCTR`（宏 `WOLFSSL_AES_COUNTER`/`WOLFSSL_AES_DIRECT`）、`WOLFSSL_SP_MATH_ALL`（大整数后端，RSA/ECC 必需）、
  `WOLFSSL_SYS_CA_CERTS off`（否则 crypt-only 下缺 `GetCA`）。
- `WOLFCRYPT_ONLY` 必须 `PUBLIC` 地传给 wolfssl 与所有消费者：它改 `RsaKey` 等结构布局，
  只给库时症状是“签名成功、验签恒假”。
- RSA 调用要点：默认 `wc_RsaPublicEncrypt`/`wc_RsaPrivateDecrypt` 是 PKCS#1 v1.5（要 OAEP 用 `_ex`）；
  `wc_RsaPSS_Verify` 只解密签名，比较要再调 `wc_RsaPSS_CheckPadding`；
  `WC_RSA_BLINDING` 下私钥操作读 `key->rng`，先 `wc_RsaSetRNG`。
- ECC 调用要点：`ECC_TIMING_RESISTANT` 下 `wc_ecc_shared_secret` 缺 `key->rng` 就报 `MISSING_RNG_E`（-236），
  先 `wc_ecc_set_rng`；`wc_ecc_sign_hash` 写的是 **DER** 签名，缓冲区要给 `2×order+16` 余量（否则 `BUFFER_E`）。- **SEC1 的公钥点是可选的**：`wc_EccPrivateKeyDecode` 遇到不带公钥点的文件会把 `key->type` 设成 `ECC_PRIVATEKEY_ONLY`，
  之后 `wc_ecc_export_x963` / `wc_EccPublicKeyToDer` 直接失败（而 `wc_ecc_verify_hash` 会自己补算，所以症状是“有时能过”）。
  `EccKey::fromPrivateDer` 因此导入后立即 `wc_ecc_make_pub_ex(&key, nullptr, rng)` 补算。
  自测时容易漏掉：验签会掩盖这个问题，导出公钥才暴露——测往返时必须调一次 `publicX963()`。
- `wc_DecryptPKCS8Key` 是**原地**解密（明文写在密文上），必须传自己复制的缓冲区，用完 `wc_ForceZero`。
- 旧式加密 PEM（`Proc-Type: 4,ENCRYPTED` + `DEK-Info:`）走 `wc_KeyPemToDer`，需要 `WOLFSSL_ENCKEYS`；
  它的标签仍是 `RSA PRIVATE KEY`，所以不能靠标签认，要看体内有没有 `Proc-Type:` 头。它会要一个 **NUL 结尾**的口令。
- 证书只取公钥：`wc_GetSubjectPubKeyInfoDerFromCert(certDer, sz, out, &outSz)`（`out` 为 NULL 时返回 0 并写 `outSz`），
  结果直接喂 `RsaKey::fromPublicDer` / `EccKey::fromPublicDer`——两个导入器本来就收 SPKI。
- **`WOLFSSL_MD5` 的默认是 disabled**，`Digest::kMd5` 的可用性不能靠预设的副作用；
  `src/base/crypto/CMakeLists.txt` 里显式 `set(WOLFSSL_MD5 yes ...)`，否则另一个构建树里 `wc_Md5` 会未声明。- 代 RNG 的公共件：`src/base/crypto/src/WolfSsl.hpp` 的 `detail::RandomHandle`（每次操作一个上下文，无锁）。
- `AesGcm` 用后端的 `wc_AesGcmEncrypt/Decrypt` 一次性入口，缓冲区自己铺；标签固定 16 字节。
- 命令：`cmake --build build --config Debug --target test_crypto` → `build\bin\Debug\test_crypto.exe`。
- 内存检查（Windows / MSVC ASAN，已验证 114 例全过无报告）：
  `cmake -S . -B build-asan -G "Visual Studio 18 2026" -A x64 -DCMAKE_BUILD_TYPE=Debug `
  `-DCMAKE_PREFIX_PATH=D:/Qt/6.8.2/msvc2022_64 -DCMAKE_CXX_FLAGS="/fsanitize=address /Zi" `
  `-DCMAKE_EXE_LINKER_FLAGS=/fsanitize=address -DCMAKE_SHARED_LINKER_FLAGS=/fsanitize=address`
  加 `-DFETCHCONTENT_SOURCE_DIR_<NAME>=build/_deps/<name>-src` 复用已下载的依赖（wolfssl / googletest / …）。
  跑之前要把 `VC\Tools\MSVC\<ver>\bin\Hostx64\x64`（放着 `clang_rt.asan_dbg_dynamic-x86_64.dll`）放进 `PATH`，
  否则进程直接 `0xC0000135`。`ASAN_OPTIONS=detect_leaks=1` 在该平台不支持，设了会直接报错退出。
