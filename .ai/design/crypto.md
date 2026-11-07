# crypto 模块

- 状态：现行
- 最后核对：2026-11-07（`test_crypto` 62 例通过；`cmake --build build --config Debug` 0 错误）
- 范围：`src/base/crypto`（SDK 根 `src/base/crypto/sdk/vine/crypto`，实现 `src/base/crypto/src`，库 `vn::Crypto`）
- 权威来源：头文件里的契约注释与 `tests/test_crypto` 的可执行行为；本文只做汇总与索引，不重复契约
- 后端：wolfSSL **v5.9.4-stable**（固定在 `src/base/crypto/CMakeLists.txt`；升级需重读其 release notes 并重跑 `test_crypto`）。

## 1. 概要设计

### 1.1 问题与背景

仓库需要一个只做算法、不做策略的加密模块：上层（appfw 及更高）要摘要、认证标签、对称加密和随机字节，
不希望自己接触第三方库的上下文初始化、内存管理和“失败时返回什么”的约定。
本模块把 wolfSSL 的选择、初始化时机和错误契约收在一层里，对外只暴露 `vn::crypto` 的词汇。

### 1.2 目标

- 每个算法只有一个家，调用者按名字找到它，不需要知道底层库（G1、G12）。
- 一次性（`hmac()` / `seal` / `bytes` / `digest()`）与增量（`update` + `finish`）两种形态成对存在，
  分块喂入的结果必须与一次性结果逐字节一致（G3、G4）。
- 失败必须显式：加密失败、认证标签不通过都走异常，绝不返回“看起来正常”的空结果（G11）。

### 1.3 非目标

- 不做密钥管理、证书链、信任策略、口令派生参数选择——这些属于 appfw 及以上。
- 不做 TLS、协议、报文格式；wolfSSL 以 `WOLFSSL_CRYPT_ONLY` 构建，协议栈不进二进制。
- 不做恒定时间以外的侧信道加固承诺；时序敏感的比较只在 `macEquals()` 一处按恒定时间实现。

### 1.4 结论

`crypto` 是“算法 + 错误契约 + 两种输入形态”的一层薄封装，内部只有一处持有后端初始化和一处持有随机源；
算法族按用途分组（摘要 / MAC / 对称加密 / 随机），新增算法只加一个类，不改既有类。

## 2. 设计要点

### 2.1 一个对象一个方向

`AesGcm` 的实例要么加密要么解密，由 `encryptor()` / `decryptor()` 两个工厂决定，没有 `mode` 标志位。
理由：加解密方向在构造时就确定，用标志位表达会允许“用加密对象解密”这类状态；工厂名本身即文档。
边界：`isEncrypting()` 只用于诊断与断言，不用于分支选择。

### 2.2 增量式必须与一次性同义

增量形态不是第二套实现：三者都直接喂后端上下文，`update()` 按块给出输出，`finish()` 收尾。
判据：同一输入按任意分块切开，结果必须逐字节相同；测试用“逐字节喂入”验证（`T2`、`T4`）。
边界：`AesGcm` 的解密输出在 `finish()` 验标签之前**不可信**，这一点写在头文件的 `@warning` 里，不藏在实现里。

### 2.3 后端初始化只有一处

wolfSSL 的全局状态（随机数状态互斥量、ASN.1 与 DRBG 表）要求 `wolfCrypt_Init()` 先跑；
未初始化就取随机数会在内部锁上崩溃（见 `P2`）。因此初始化只放在 `detail::ensureLibraryInitialized()`，
`std::call_once` 保证一次成功、失败可重试，其它文件一律不直接调用后端初始化。

### 2.4 失败不交出不认证的数据

`AesGcm::finish()` 在解密路径上就是标签判定：成功说明 `update()` 交出去的每个字节都真实，失败说明全部作废；
`Hash` 系列在底层失败时返回全零数组，因为摘要没有“认证”语义，返回状态会污染调用方的流程（G11）。

### 2.5 名字归一，大小写与分隔符不敏感

`digestFromName()` 去掉 `-`、`_` 并转小写，所以 `"SHA-256"`、`"sha256"`、`"sha3_512"` 都能识别；
未知名字抛 `std::invalid_argument`，不做静默回退到默认算法。

### 2.6 随机源只有一个

`Random` 持有进程级的 DRBG 上下文与一把互斥锁，大小请求按 32 位分块喂给后端。
理由：DRBG 自己会重播种，多个上下文只会多花钱；代价是串行化，而该 API 的使用规模是密钥、nonce、盐。

### 2.8 摘要只有一套词汇、两种形态

`Digest` 枚举是“哪个哈希”的唯一拼法：一次性 `digest(Digest, span)`、增量 `Hasher`、
`Hmac`、`RsaKey`、`EccKey` 全部用它选算法。
理由：先前 `Hash`（每个算法一个静态函数）、`HashCalculator::ALG_*`、`Hmac::Algorithm` 三套并存，
同一概念三个名字（G1、G6）；且返回类型按算法写死，调用方写不出与算法无关的代码。
边界：**不按算法拆类**（`class Sha256` 之类）——所有算法的形状完全相同，拆开只会多出 N 个同形类型
与一层运行期选择（G8）。一次性与增量共用 `DigestOps` 的一份后端上下文，实现只写一遍。

### 2.7 非对称：一把钥匙一个对象，操作自己带上下文

### 2.7 非对称：一把钥匙一个对象，操作自己带上下文
`RsaKey` 的实例就是一把钥匙：生成或导入得到带私钥的对象，只导入 `n`/`e` 得到的对象只能验签与加密。
每次需要随机性的操作（生成、PSS 签名、加解密）自己申请一个后端 RNG 上下文，不在钥匙里长期持有：
理由：共享的 RNG 要配锁、要管生命周期，而这里的频率是“每次签名一次”。
边界：`sign()` / `decrypt()` 在只有公钥时抛 `std::logic_error`，不做退化成“算不了就给错结果”。

## 3. 功能点

| 编号 | 功能 | 入口 |
|---|---|---|
| F1 | 密码学随机字节 | `Random::fill` / `Random::bytes` |
| F2 | HMAC 一次性与增量 | `hmac()` / `Hmac` |
| F3 | 恒定时间标签比较 | `macEquals()` |
| F4 | AES-GCM 一次性 | `AesGcm::seal` / `AesGcm::open` |
| F5 | AES-GCM 流式（有界内存） | `AesGcm::encryptor` / `decryptor` + `update` / `finish` |
| F6 | 摘要：一次性与增量（唯一词汇） | `digest()` / `Hasher` |
| F8 | ECDSA 签名与 ECDH 密钥协商 | `EccKey` |
| F9 | HKDF 提取与扩展 | `Hkdf::extract` / `expand` / `derive` |
| F10 | AES-CBC（PKCS#7，仅为互通） | `AesCbc` |
| F11 | AES-CTR（调用方管计数器） | `AesCtr` |
| F12 | PBKDF2 口令哈希（互通） | `Pbkdf2::derive` |
| F13 | Argon2 口令哈希（推荐） | `Argon2::derive` |
| F14 | 口令保护的私钥（PKCS#8 加密与旧式 `Proc-Type` 两种容器） | `RsaKey` / `EccKey` 的 `toEncryptedPrivateDer` / `toEncryptedPrivatePem` / `fromEncryptedPrivateDer` / `fromEncryptedPrivatePem` |
| F15 | 从 X.509 证书取公钥 | `RsaKey::fromCertificateDer/Pem`、`EccKey::fromCertificateDer/Pem` |

### 3.1 F1 密码学随机字节

行为：`fill(std::span<std::byte>)`、`fill(void*, size_t)`、`bytes(size)` 从进程级 DRBG 取值。
约束：`C1`、`C2`；要求：`R1`；验收：`T1`。
说明：`size == 0` 是空操作（`nullptr` 也接受）；`nullptr` 配非零长度抛 `std::invalid_argument`。
注意：`N1` —— 失败时绝不返回部分填充的缓冲区；不可用等“可接受不随机”的情况不存在，一律抛异常。

### 3.2 F2 HMAC 与 F3 恒定时间比较

行为：`hmac(Digest, key, data)` 一次性给出标签；`Hmac` 按 `update` / `finish` 增量给出同样的标签。
约束：`C3`；要求：`R2`；验收：`T2`。
说明：`finish()` 可重复调用，第二次返回缓存值而不是再次终结上下文；`update()` 在 `finish()` 之后抛 `std::logic_error`。
`verify` 在长度不同时直接 `false`，长度相同时按位异或累加，不提前返回。
注意：`N2` —— `macEquals()` 的长度检查不是恒定时间的（见 `P6`）；标签长度不是秘密时才可用。

### 3.3 F4 / F5 AES-GCM

行为：`seal(key, iv, plaintext, aad)` 返回 `{ciphertext, tag}`；`open(...)` 验证标签后返回明文；
`encryptor` / `decryptor` 接受任意分块输入，`finish()` 给出输出，加密侧再用 `tag()` 取标签。
约束：`C4`、`C5`、`C14`；要求：`R3`；验收：`T3`、`T4`。
说明：密文长度等于明文长度，标签 16 字节另行存放；`AAD` 只认证不加密。
注意：`N3` —— 解密时 `update()` 的输出在 `finish()` 成功前不可信（`C14`、`P4`）；同一密钥下 IV 绝不可重复（`P5`）。

### 3.4 F6 摘要

行为：`digest(Digest, span<const std::byte>)` 一次性算出摘要；`Hasher` 按 `update` / `finish` 增量算出同样字节；
`digestSize` / `digestName` / `digestFromName` 提供长度与名字（大小写、`-`、`_` 均忽略）。
约束：`C6`；要求：`R4`；验收：`T5`。
说明：`ByteSequenceFingerprint` 是建在其上的值类型指纹（11 种 9 算法不重复）；旧 `Hash` / `HashCalculator`已删除。
注意：`N4` —— 摘要不得当作 MAC 用（`Hmac` 才是），两回事。

### 3.5 F7 RSA

行为：`RsaKey::generate(bits)` 生成密钥；`fromPrivateDer` / `fromRawPublic` 导入，`toPrivateDer` 导出；
`sign()` / `verify()` 支持 PSS（默认）与 PKCS#1 v1.5 两种填充；`encrypt()` / `decrypt()` 走 OAEP + SHA-256。
约束：`C7`~`C9`；要求：`R5`；验收：`T6`。
说明：签名先把消息哈希成 SHA-256 摘要，签名长度等于模长；公钥密钥只能验签和加密。
密钥可存为 DER（PKCS#1 私钥 / SubjectPublicKeyInfo 公钥）或 PEM（`RSA PRIVATE KEY` / `PUBLIC KEY`）。
注意：`N5` —— 后端有三处默认值与直觉相反，见 `P9`~`P11`（默认 PKCS#1 v1.5 padding、PSS verify 只解密不比较、盲化需要 `key->rng`）。

### 3.6 F8 ECC

行为：`EccKey::generate(curve)` 生成 P-256 / P-384 密钥；`fromPrivateDer` / `fromPublicX963` 导入，
`toPrivateDer` / `publicX963()` 导出；`sign()` / `verify()` 是 ECDSA（P-256 配 SHA-256，P-384 配 SHA-384）；
`sharedSecret(peer)` 是 ECDH。
约束：`C10`、`C11`；要求：`R6`；验收：`T7`。
说明：签名的字节形态是 DER 编码的 `ECDSA-Sig-Value`（与 openssl 一致），长度随内容在 70–72 / 102–104 之间浮动。
注意：`N6` —— ECDH 的结果是密钥材料，不是可直接使用的密钥；必须先过 `Hkdf::derive()`（见 F9）才能当对称密钥。
公钥有两种导出：`publicX963()`（裸点，供协议用）与 `toPublicDer()` / `toPublicPem()`（SubjectPublicKeyInfo）。

### 3.7 F9 HKDF

行为：`Hkdf::derive(digest, ikm, salt, info, length)` 一次完成 extract + expand；
`Hkdf::extract()` 与 `Hkdf::expand()` 单独暴露，供 salt/PRK 已被协议固定的场景。
约束：`C12`；要求：`R7`；验收：`T8`。
说明：输出上限是 `255 × digestSize`（RFC 5869），超限抛 `std::invalid_argument`；salt 与 info 可为空。
注意：`N7` —— HKDF 不是口令哈希：它设计上很快，口令必须走慢 KDF（见 F12、F13）。

### 3.8 F10 / F11 AES-CBC 与 AES-CTR

行为：`AesCbc` 与 `AesCtr` 都是 `create/encryptor` → `update()` → `finish()` 的流式对象，另有一次性 `seal`/`open`
（CBC）与 `apply`（CTR）。CBC 自带 PKCS#7 填充与去填充；CTR 无填充，输出长度恒等于输入。
约束：`C15`；要求：`R8`；验收：`T11`。
说明：两者都是**互通专用**：CBC 没有认证、CTR 连填充都没有，安全性完全靠调用方再配 MAC。默认仍然用 `AesGcm`。
注意：`N8` —— CBC 的填充错误会变成可观测的差异（`P19`）；CTR 的计数器由调用方负责（`P20`）。

### 3.9 F12 / F13 口令哈希

行为：`Pbkdf2::derive(algorithm, password, salt, iterations, length)` 与
`Argon2::derive(variant, password, salt, parallelism, memory, iterations, length)`；两者都只做派生，不做验证（验证 = 重新派生 + `macEquals()`）。
约束：`C16`、`C17`；要求：`R9`；验收：`T12`。
说明：新口令用 Argon2id（内存硬）；PBKDF2 只因为格式/对端已规定而存在，且 salt 只要求非空（RFC 6070 的向量就用 4 字节盐）。
注意：`N9` —— 两者都是故意慢的，不要放在 UI 线程；参数要按部署机器实测选定并随凭据一起存。

### 3.10 F14 口令保护的私钥

行为：`toEncryptedPrivateDer(password, iterations)` / `toEncryptedPrivatePem(password, iterations)` 把私钥写成 PKCS#8
`EncryptedPrivateKeyInfo`（PBES2 + AES-256-CBC + PBKDF2，随机盐与随机 IV）；`fromEncryptedPrivatePem` / `fromEncryptedPrivateDer`
读回来并直接导成 `RsaKey` / `EccKey`。四个方法各自一行：真正的编解码在 `detail::encryptPkcs8Der` / `detail::decryptPkcs8Der`，
两种密钥共用（G5：口令加解密只应有一份实现）。
约束：`C18`；要求：`R10`；验收：`T13`。
说明：传统 DER（PKCS#1 / SEC1）没有放口令的地方，加密只能靠外层结构，所以这里产出的永远是 PKCS#8，而不是“给旧格式加个口令”。
读的方向接受 PBES2 与更老的 PBES1 / PKCS#12 口令方案——参数都在结构里，只需要口令。
注意：`N10` —— 口令保护的文件是**离线的**口令校验器，没有任何东西在限制尝试速度（`P21`）。
把口令保护的块喂给 `fromPrivatePem` 会直接报错并指向 `fromEncryptedPrivatePem`（`detail::pemIsEncryptedPrivateKey` / `detail::pemIsLegacyEncrypted` 先看标签），
而不是让 ASN.1 解析器报一个“这不是私钥”的错。

读的方向还接受**旧式**形式：`RSA PRIVATE KEY` / `EC PRIVATE KEY` 标签不变，参数放在体内的 `Proc-Type: 4,ENCRYPTED` 与
`DEK-Info: <cipher>,<iv>` 头里（2020 年前 `openssl rsa -aes128` 的产物）。这一形状的标签在说谜，不能靠标签识别，
要先看头部（`detail::pemIsLegacyEncrypted`），解密交给后端的 `wc_KeyPemToDer`（`detail::pemDecryptLegacy`）。
注意：`N11` —— 旧式形式的密码套件由 `DEK-Info` 指定，能否解开取决于构建里启用的是哪些算法（`P23`）。

### 3.11 F15 从 X.509 证书取公钥

行为：`RsaKey::fromCertificatePem/Der` 与 `EccKey::fromCertificatePem/Der` 把证书里的 `SubjectPublicKeyInfo` 取出来，
再走已有的公钥导入器。实现只有 `detail::certificatePublicKeyDer`（`src/Certificate.cpp`，一次
`wc_GetSubjectPubKeyInfoDerFromCert`），所以两个类各自是一行委托。
约束：`C19`；要求：`R11`；验收：`T14`。
说明：本层只把证书当成“包裹着公钥的容器”。签名、有效期、issuer、SAN、信任链都是策略，属于 appfw；模块不做判断。
一个包内只有**第一张**证书会被读取——从链里选哪一张是调用方的决定，不是算法能代决的。
注意：`N12` —— 证书的密钥类型不对时，报错来自公钥导入器（“不是 RSA 公钥”），而不是“不是证书”。

## 4. 公开接口

| 位置 | 类型 | 形态 |
|---|---|---|
| `vine/crypto/Random.hpp` | `Random` | 全静态，无状态 |
| `vine/crypto/Hmac.hpp` | `hmac()` / `macEquals()` 自由函数 + `Hmac` 类 | 一次性是函数，增量对象 PImpl、可移动、不可拷贝 |
| `vine/crypto/AesGcm.hpp` | `AesGcm`、`AesGcm::Sealed` | 工厂 + PImpl、可移动、不可拷贝 |
| `vine/crypto/Digest.hpp` | `Digest` + `digest()` / `digestSize()` / `digestName()` / `digestFromName()` | 枚举 + 自由函数 |
| `vine/crypto/Hasher.hpp` | `Hasher` | PImpl、增量、可移动 |
| `vine/crypto/ByteSequenceFingerprint.hpp` | `ByteSequenceFingerprint` | 值类型 |
| `vine/crypto/RsaKey.hpp` | `RsaKey`、`RsaKey::Scheme` | 工厂 + PImpl、可 移动、不可拷贝；工厂含私钥/公钥/口令保护/证书四类 |
| `vine/crypto/EccKey.hpp` | `EccKey`、`EccKey::Curve` | 工厂 + PImpl、可移动、不可拷贝；同上四类
| `vine/crypto/Hkdf.hpp` | `Hkdf` | 全静态（extract / expand / derive） |
| `vine/crypto/AesCbc.hpp` | `AesCbc` | 工厂 + PImpl、流式（`update` / `finish`） |
| `vine/crypto/AesCtr.hpp` | `AesCtr` | 工厂 + PImpl、流式 |
| `vine/crypto/Pbkdf2.hpp` | `Pbkdf2` | 全静态 |
| `vine/crypto/Argon2.hpp` | `Argon2`、`Argon2::Variant` | 全静态 |
| `vine/crypto/crypto_global.hpp` | 宏 | `VN_CRYPTO_API`、命名空间宏 |

契约细节（参数约束、抛什么、返回什么）只在头文件里写一份，本文不复述。

## 5. 约束、要求与不变量

- `C1`：密钥长度只接受 16 / 24 / 32 字节；其它长度在构造期抛 `std::invalid_argument`。
- `C2`：IV 不得为空；GCM 的 12 字节是推荐长度，1~16 字节之外的组合由后端拒绝。
- `C3`：HMAC 密钥长度不受限制（后端按需哈希长密钥），但标签长度由算法决定，不可截断。
- `C4`：一个 `AesGcm` 实例只能 `finish()` 一次；第二次抛 `std::logic_error`。
- `C5`：`tag()` 只在加密对象上、且在 `finish()` 之后可用，其它情况抛 `std::logic_error`。
- `C6`：`digest()` 失败抛 `std::runtime_error`（不再返回全零数组）；增量形态同样抛异常，不静默答 0。
- `C7`：`RsaKey` 的模长必须是 8 的倍数且落在 [2048, 8192] 内；公钥指数固定 65537。
- `C8`：`RsaKey::encrypt()` 的明文上限是 `模长 − 66` 字节（OAEP + SHA-256 的开销）；超长抛 `std::invalid_argument`。
- `C9`：签名与加密方案不可混用：PSS 签名不能用 PKCS#1 v1.5 验，OAEP 密文不能用 v1.5 解。
- `R1`：`Random` 必须可用于生成密钥、nonce、盐；不得在任何失败路径上返回部分填充的缓冲。
- `R2`：增量与一次性的结果必须逐字节相同，分块大小为 1 时也成立。
- `R3`：解密的明文只在标签验证通过后才离开模块。
- `R4`：`ByteSequenceFingerprint` 的公开接口不变（它已改建立在新的 `Digest` 词汇上）。
- `R5`：`RsaKey` 的验签与解密失败一律答 `false` 或抛异常，不得返回长度可疑的明文。
- `C10`：`EccKey` 只支持 P-256 与 P-384；ECDH 要求两侧同一曲线，不同曲线抛 `std::invalid_argument`。
- `C11`：ECDSA 签名是 DER 编码，长度不固定；调用方不得假设 64/96 字节的定长。
- `C12`：HKDF 的输出长度必须在 1 到 `255 × digestSize` 之间；`extract` 与 `expand` 必须用同一个 digest。
- `C13`：PEM 只读写**未加密**的块；口令保护的私钥不在支持范围（`wc_KeyPemToDer` 能解，但需要口令来源与错误语义，另议）。
- `C14`：`AesGcm` 一次只处理一块：`update()` 的输出只属于本次输入，不缓冲、不追回；解密输出在 `finish()` 成功前不可信。
- `C15`：`AesCbc` 的 IV 固定 16 字节、密文必须是 16 的整倍数；`AesCtr` 的初始计数器块固定 16 字节。
- `C16`：`Pbkdf2` 的 salt 不得为空、`iterations ≥ 1`、输出长度 ≥ 1（`length` 不设上限，由调用方为内存负责）。
- `C17`：`Argon2` 的 salt ≥ 8 字节、`parallelism ≥ 1`、`memory ≥ 8 × parallelism`、`iterations ≥ 1`、输出 ≥ 4 字节。
- `C18`：口令保护的私钥不接受空口令；`iterations` 必须落在 `1 .. INT_MAX`，因为它会原样写进 PBKDF2 参数。
- `C19`：证书导入只接受单张证书的 DER / PEM；空输入与结构不符的字节一律 `std::invalid_argument`，不做部分解析。
- `R6`：ECDH 两边必须得到逐字节相同的秘密；签名必须能被公钥单独验证。
- `R7`：`Hkdf::derive` 必须与“先 extract 再 expand”逐字节一致；不同 `info` 必须给出不相关的输出。
- `R8`：`AesCbc` 的流式输出必须与一次性结果逐字节相等，且填充必须能被自验证拒绝（非法填充报错而不是静默截断）。
- `R9`：相同的（口令、盐、参数）必须派生出相同字节；改动任一项必须得到不同结果；与 RFC 向量逐字节一致。- `R10`：`toEncryptedPrivatePem` 的产物必须能被 OpenSSL 读回并解出同一把公钥；反之，OpenSSL 写出的加密私钥必须能被读入；
  口令错误必须报错而不是给出半个密钥。
- `R11`：从 OpenSSL 生成的证书取出的公钥，必须与 `openssl x509 -pubkey` 给出的公钥逐字段一致。
## 6. 注意事项与陷阱

- `P1`：wolfSSL 头文件把 `Md5`、`Sha256`、`Sha384` 等定义成宏（`#define Md5 wc_Md5`），
  自己的枚举若用同名枚举值会在 `.cpp` 里被宏替换成类型名，编译器报“成员声明中的限定名称非法”。
  枚举值一律用 `k` 前缀（`Digest::kSha256`），既符合仓库常量命名，也天然避开。
  检测：包含 wolfSSL 头文件后编译失败且报错指向枚举值；预防：不要在命名空间里用 `Sha256` 这类裸名字。
- `P2`：`wc_InitRng()` 内部会锁 DRBG 状态互斥量，而该互斥量只有 `wolfCrypt_Init()` 会初始化；
  跳过初始化会在 `wc_LockMutex` 里访问违例（本仓库实测 0xc0000005）。
  检测：首次 `Random::fill` 崩溃且栈上是 `wc_InitRng → wc_LockMutex`；预防：只经 `detail::ensureLibraryInitialized()`。
- `P3`：`vn::crypto::Hmac` 与 wolfSSL 的全局 `::Hmac` 同名。在 `vn::crypto` 里写 `Hmac x;` 声明的是本模块的类型，
  后端上下文必须写 `::Hmac`。检测：`wc_HmacInit` 的参数类型不匹配；预防：PImpl 里显式写 `::Hmac`。
- `P4`：AES-GCM 的解密是“先出后验”：`update()` 一开始就吐明文，而要到最后一块才知道标签对不对。
  所以**写到最终目的地之前必须等 `finish()` 成功**（或写到可回滚的地方），否则就是把未认证字节落了盘。
  检测：任何在 `finish()` 之前提交输出的调用点；预防：读入 → 写临时文件/内存 → 校验通过后重命名/提交。
- `P5`：GCM 的 IV 在同一密钥下重复使用会彻底破坏机密性与认证；随机 IV 要遵守 12 字节与生日界，
  计数器 IV 只有在调用方能保证不回绕时才可安全使用。
- `P6`：`macEquals` 的长度检查不是恒定时间的：长度不同立即返回，长度相同才逐字节比较；长度不是秘密时这是可接受的。
  逐字节部分用后端的 `wc_ConstantCompare()`（5.9.4 起公开），不再自己写循环。
- `P7`：FetchContent 拉下来的 wolfSSL **只能用它的 CMake 选项名打开功能**，写成宏名会静默失效：
  `WOLFSSL_KEY_GEN`（宏名）不生效，选项名是 `WOLFSSL_KEYGEN`；AES-CTR 的选项名是 `WOLFSSL_AESCTR`。
  写错时 `options.h` 里会看到宏，库自己的编译定义里却没有 → `rsa.c` 不导出 `wc_MakeRsaKey`（LNK2019）。
- `P8`：`WOLFSSL_CRYPT_ONLY` 只裁源文件列表，不定义宏。库源码靠 `WOLFCRYPT_ONLY` 判断，且这个宏
  **会改 `RsaKey` 等结构体的布局**，所以必须 `PUBLIC` 地传给所有消费者；否则症状是“签名成功、验签永假”。
  同一原因下 `WOLFSSL_SYS_CA_CERTS` 必须关：`asn.c` 会引用只有 TLS 层才定义的 `GetCA/GetCAByName`。
- `P9`：后端默认 `wc_RsaPublicEncrypt` / `wc_RsaPrivateDecrypt` 用 **PKCS#1 v1.5**，本模块要 OAEP 就必须
  显式调 `_ex` 版本并传 `WC_RSA_OAEP_PAD` + SHA-256 + MGF1-SHA-256。
- `P10`：`wc_RsaPSS_Verify()` 只**解密签名**并返回 PSS 数据，不做比较；必须再调 `wc_RsaPSS_CheckPadding()`。
  直接拿它的返回值当“恢复出的摘要”会让验签恒为假。
- `P11`：本构建开了 `WC_RSA_BLINDING`，后端在私钥操作里读 `key->rng`；没绑 RNG 就解不了密。
  每次操作前调 `wc_RsaSetRNG()`，不要指望 `wc_InitRsaKey()` 会替你填。
- `P12`：ECC 同理且更容易踩：`ECC_TIMING_RESISTANT` 下 `wc_ecc_shared_secret()` 直接要求
  `key->rng`，缺失就返回 `MISSING_RNG_E`（-236）；必须先 `wc_ecc_set_rng()`。
- `P14`：wolfSSL 的 SHA-3 没有通用入口：`wc_Sha3_224_Update` / `_Final` / `_Free` 四个变体各一套，
  写 `wc_Sha3Update` 这类名字会直接编译失败。四个变体共享 `wc_Sha3` 上下文类型，只是入口不同。
- `P15`：SDK 里的自由函数必须标 `VN_CRYPTO_API`。漏标不会编译报错，直到别的库链接时才会出 LNK2019。
- `P18`：`RsaKey` / `EccKey` 的**一个对象不属于多线程**：验签与密钥交换会把每次操作的 RNG 绑进后面背景上下文
  （`key->rng`），两个线程共用一个对象就会在这个字段上相撞。`Random` / `Hasher` / `Hmac` / `AesGcm` 不受此限（后者各自持有一份上下文）。
- `P19`：CBC 的填充错误是**可观测的**：攻击者能区分“填充不对”与“填充对但明文是垃圾”，这就是填充预言攻击。
  本模块的 `open()` / `finish()` 对这两种情况都报同一个错误文本，但时序上仍然有差异，所以结论是：**不要用裸 CBC 解不可信数据**。
- `P20`：CTR 的计数器重复使用等于把两段明文异或后交给攻击者；它也没有认证，翻转密文位就翻转明文位。
  `AesCtr` 只提供机制，不提供保护——需要认证就用 `AesGcm`。
- `P21`：口令保护的私钥文件就是它的口令校验器：拿到文件就能离线反复尝试，没有任何速率限制。
  检测：不存在“耐心一点就能救回弱口令”的情形。预防：默认 200000 次 PBKDF2 迭代只是抬高单价，真正的防线是口令强度。
- `P22`：SEC1 的**公钥点是可选的**。`wc_EccPrivateKeyDecode` 在文件不带公钥点时把密钥标成 `ECC_PRIVATEKEY_ONLY`，
  之后 `publicX963()` / `toPublicDer()` / ECDH 都直接失败（而 `verify()` 会自己补算，于是看上去“有时能过”）。
  `EccKey::fromPrivateDer` 因此在导入后立即用 `wc_ecc_make_pub_ex` 补算公钥点。
- `P23`：旧式加密 PEM 的 `DEK-Info` 可以点出构建里没有的算法（RC2、DES-EDE3 …）。
  `wc_KeyPemToDer` 对这种情况返回负值，与错口令无法区分；报错文案因此写的是“口令错或不支持的旧式编码”。
- `P24`：变异测试测到一个值得记住的事实：口令保护的私钥文件里，**约四分之一**的单字节变异仍会被接受（成为另一把密钥）。
  原因不是导入器有缺陷，而是 PBES2 是 CBC、没有 MAC，而 RSA 密钥的主体是不透明整数内容——翻一个字节结构仍合法。
  检测：`.ai/memory/crypto.md` 的 ASAN 命令可以重现；预防：需要篡改检测就用带认证的容器，而不是口令加密的 PKCS#8。
- `P16`：wolfSSL 的 PEM 解码入口 `wc_KeyPemToDer()` 内部写死了 `PRIVATEKEY_TYPE`，**解不了 `PUBLIC KEY` 块**；
  它还只认自家编码的细节。本模块因此自己做装甲剥离 + base64 解码（`src/Pem.cpp`），编码仍用 `wc_DerToPem()`：
  这样公钥 PEM 与私钥 PEM 走同一条路，标签是否匹配由后面的 DER 解码器判定并给出准确错误。
- `P17`：MSVC 上 `HAVE_GETPID` 会被 wolfSSL 的 `check_symbol_exists(getpid "unistd.h")` 误判为存在
  （只链接静态库的工具链会把每个符号都报为找到），于是 `random.h` 在 `WC_RNG` 里声明 `pid_t pid;` 而 Windows 没有 `pid_t`。
  `src/base/crypto/CMakeLists.txt` 在 `FetchContent_MakeAvailable` 之前把 `HAVE_GETPID` 预置为 `OFF`（缓存变量让探测直接跳过）。
  检测：编译报 `random.h(...): 语法错误: 标识符"pid_t"`；预防：升级 wolfSSL 后先确认这条预置还在。

## 7. 已否决选项

- `D1`：让 `AesGcm` 同时承担加解密并用 `mode` 标志位区分。否决：状态多一种、错误多一类，见 `ADR-2026-11-07-crypto-api-shape.md`。
- `D2`：直接把 wolfSSL 的增量 GCM 入口暴露成流式。**已采纳**（原否决理由不成立）：5.9.4 提供了
  `wc_AesGcm{Encrypt,Decrypt}{Init,Update,Final}` 这套稳定入口，且 `DecryptFinal` 带 `WARN_UNUSED_RESULT`，
  “调错就放行未认证数据”的风险由“必须用返回值” + 头文件契约覆盖；见 `ADR-2026-11-07-aesgcm-streaming.md`。
- `D3`：标签比较用 `std::equal` / `memcmp`。否决：提前返回会泄漏差异位置。
- `D4`：`Random` 每次调用新建一个上下文。否决：多付初始化与播种代价，且没有换来任何隔离收益。
- `D5`：把 HMAC 的算法枚举改成 `Hmac::Algorithm` 之外的公共 `Digest` 词汇。**已采纳**：`Digest` 现在是全模块唯一的摘要词汇，
  `Hash` / `HashCalculator` / `Hmac::Algorithm` 三套并行词汇已删除，见 `ADR-2026-11-07-digest-vocabulary.md`。
- `D7`：保留 `*Calculator` 这种命名（自造词，业界无此叫法）。否决：改为“一次性用自由函数、增量用东西本身命名”的形态——
  `digest()` / `Hasher`、`hmac()` / `Hmac`，与 OpenSSL、libsodium、RustCrypto、Qt 一致；见 `ADR-2026-11-07-incremental-api-shape.md`。
- `D6`：每种摘要一个类（`class Sha256` …）或用继承抽公共基类。否决：各算法形状完全相同，拆开只多 N 个同形类型，
  且运行期选择还得再加一层虚调用；`enum + 一张表` 同样安全、更小。

## 8. 测试与验收

| 编号 | 判据 | 用例 / 命令 |
|---|---|---|
| `T1` | 两次 32 字节取值不相同，且出现非零字节；空请求是空操作；`nullptr` + 非零长度抛异常 | `RandomTest.*` |
| `T2` | RFC 4231 case 2 的 HMAC-SHA256 向量匹配；逐字节喂入等于一次性；名字归一化；长度不符的 `verify` 为假 | `HmacTest.*` |
| `T3` | NIST SP 800-38D case 2 的密文与标签匹配；标签/AAD/密文任一被改都抛异常 | `AesGcmTest.MatchesNistSp80038dCase2`、`RejectsATamperedTagOrAad` |
| `T4` | 1000 字节按 1 + 509 + 余下分块加密＝一次性密文；分块解密还原；篡改任一字节或标签都在 `finish()` 抛异常；空输入仍认证 AAD | `AesGcmTest.SealsAndOpensAChunkedMessage`、`StreamingDecryptRejectsATamperedChunkOrTag`、`HandlesEmptyInput` |
| `T5` | 9 种摘要的已知向量、空输入、增量与一次性一致、名字归一化、非法名字抛异常；`ByteSequenceFingerprint` 用例继续通过 | `CryptoTest.*` |
| `T6` | 2048 位密钥：PSS 与 PKCS#1 v1.5 各自的签验/互换失败、OAEP 往返、DER 往返、跨钥拒绝、非法导入拒绝 | `RsaKeyTest.*` |
| `T7` | P-256 与 P-384 的签验、篡改拒绝、DER 往返、公有点验签与“公钥不能签”、ECDH 两侧一致、跨曲线拒绝 | `EccKeyTest.*` |
| `T8` | RFC 5869 test case 1 的 PRK 与 OKM 逐字节匹配；derive == extract+expand；空 salt/info 可用；越界长度抛异常 | `HkdfTest.*` |
| `T9` | 私钥/公钥 PEM 往返（标签正确、能从 PEM 验签、公钥 PEM 不能当私钥用）、非法 PEM 抛异常 | `RsaKeyTest.RoundTripsThroughPem`、`EccKeyTest.RoundTripsThroughPem` |
| `T10` | 加固层：并发取随机不重复、AES 全合法密钥/IV 组合、非法参数拒绝、增量对象移动后续算、HKDF 边界长度、PEM 残缺/错标签/非 base64、跳凿/跳曲线拒绝 | `CryptoRobustnessTest.*` |
| `T11` | CBC 对 NIST SP 800-38A F.2.1 向量（前四块）与 0..40 字节全长度填充；分块在 1/15/17/31 字节处与一次性一致；非法长度与坏填充报错。CTR 对 F.5.1 向量，任意分块（含逐字节）与一次性一致 | `AesCbcTest.*`、`AesCtrTest.*` |
| `T12` | PBKDF2 对 RFC 6070 的四个向量；Argon2 确定性与参数敏感性（变体/内存/迭代/盐）与参数下限 | `Pbkdf2Test.*`、`Argon2Test.*` |
| `T13` | 口令保护的私钥：RSA 与 ECC（P-256/P-384）PEM 与 DER 往返、签名/ECDH 在往返后仍成立、两个相同输入产出不同密文、密文不含明文私钥、必含 PBES2 与 AES-256-CBC 的 OID、错口令/空口令/0 迭代/公钥导出被拒、OpenSSL 生成的固定夹具能解密、口令保护的块喂给 `fromPrivatePem` 时报错并指向 `fromEncryptedPrivatePem`；旧式 `Proc-Type` 夹具能解密且错口令被拒 | `EncryptedRsaKeyTest.*`、`EncryptedEccKeyTest.*` |
| `T14` | 从 OpenSSL 生成的 RSA 与 P-256 证书取出的公钥与 `openssl x509 -pubkey` 一致；密钥类型不符与“标签对但内容不是证书”均被拒；证书包只读第一张 | `CertificateTest.*` |
| `T15` | 解析入口的变异测试：私钥/公钥 DER、X9.63 点、加密私钥、PEM 文本各 240 个固定种子变异体，断言不崩溃、被接受的结果必须是一把可用的密钥；GCM 对被篡改的密文/标签一个也不放行 | `ParserMutationTest.*` |

命令：`cmake --build build --config Debug --target test_crypto`，然后 `build\bin\Debug\test_crypto.exe`。

内存检查（本机，MSVC AddressSanitizer）见 `.ai/memory/crypto.md` 里的 `build-asan` 命令；目前 114 个用例全部通过且无 ASAN 报告。

## 9. 状态与演进

- 已实现：摘要、HMAC（一次性 + 增量 + 恒定时间校验）、AES-GCM（一次性 + **有界内存流式**）、随机字节、
  后端初始化、RSA（生成 / DER 与 PEM 导入导出 / PSS 与 PKCS#1 v1.5 签名 / OAEP 加解密）、
  ECC（P-256 / P-384、ECDSA、ECDH、DER / X9.63 / PEM 导入导出）、HKDF、AES-CBC、AES-CTR、PBKDF2、Argon2、
  口令保护的私钥（PKCS#8 + PBES2，以及旧式 `Proc-Type` 读取）、从 X.509 证书取公钥。
- 已知缺口：无功能项。证书的**其它事实**（有效期、Subject、SAN、信任链）不是缺口而是边界：它们属于 appfw 的策略层，
  本模块只把证书当成“包裹着公钥的容器”（`G13` 式的分工）。其余工程项见 `.ai/backlog/crypto.md`。
- 兼容性：wolfSSL 现在按真实的 crypt-only 配置构建（`P7`、`P8`），Debug 静态库 `wolfssld.lib` 从 2.2 MB 量级增大，
  因为它第一次包含了大整数与 RSA/ECC 实现；静态煮进 `VineCrypto`，对外无 ABI 影响。

## 10. 参考

- 代码：`src/base/crypto/sdk/vine/crypto/{Digest,Hasher,Hmac,Hkdf,AesGcm,AesCbc,AesCtr,Pbkdf2,Argon2,RsaKey,EccKey,Random}.hpp`、
  `src/base/crypto/src/{Digest,DigestOps,Hasher,Hmac,Hkdf,AesGcm,AesCbc,AesCtr,Pbkdf2,Argon2,RsaKey,EccKey,Random,WolfSsl,Pem,Pkcs8,Certificate}.cpp`
- 使用者文档：`docs/guides/crypto.md`
- 决策：`ADR-2026-11-07-crypto-api-shape.md`
- 待办：`.ai/backlog/crypto.md`
- 外部：RFC 2104（HMAC）、RFC 4231（HMAC-SHA2 测试向量）、NIST SP 800-38D（GCM）、RFC 5869（HKDF）、RFC 8017（RSA）、
  RFC 6090/6979（ECDSA）、wolfSSL v5.9.4 手册
