# crypto 模块使用说明（`vn::Crypto`）

这份文档写给**调用方**：要在自己的代码里算摘要、做认证、加密数据、签名、存密钥时，先读它。
模块内部的规则、否决过的方案与陷阱在 [`.ai/design/crypto.md`](../../.ai/design/crypto.md) 里，这里只讲怎么用。

一句话概括：这个层**只做算法**——给它字节，它还你字节。它不决定信任谁、不存密钥、不管理证书生命周期；
那些属于 appfw 的策略层。

## 1. 选型：要做什么，用哪个

| 你要做的事 | 用这个 | 别用那个 |
| --- | --- | --- |
| 校验一段数据没被改 | `AesGcm`（有密钥）、`digest()` + 自己的比法（无密钥、只要指纹） | 裸 `AesCbc` / `AesCtr`：它们不认证 |
| 加密一段数据 | `AesGcm::seal` / `open` | CBC/CTR 只在对接既有格式时用 |
| 加密**大文件或流** | `AesGcm::encryptor` / `decryptor` + `update` / `finish`（内存有界） | 一次性 `seal`（整块进内存） |
| 带密钥的完整性标签 | `hmac()` / `Hmac` + `macEquals()` | 手写比较（会泄露时序） |
| 算文件指纹、缓存键 | `digest(Digest::kSha256, bytes)` | MD5 / SHA-1（只用于互通旧格式） |
| 从共享秘密派生多个密钥 | `Hkdf` | 直接 `digest()` 秘密（没有分离性） |
| 存**口令** | `Argon2`（新数据）；`Pbkdf2` 仅对接规定死格式 | `Hkdf`、裸 `digest()`：它们故意很快，正好是口令最不能要的性质 |
| 签名 / 验签 | `RsaKey`（PSS）或 `EccKey`（ECDSA） | PKCS#1 v1.5 仅为旧对端保留 |
| 加密一段短秘密（会话密钥、口令） | `RsaKey::encrypt` / `decrypt`（OAEP） | 不要拿 RSA 加密业务数据 |
| 双方协商共享秘密 | `EccKey::sharedSecret` | 自己拼 ECDH |
| 口令保护私钥文件 | `toEncryptedPrivatePem` / `fromEncryptedPrivatePem` | 自己拼加密容器（别的工具读不了） |
| 从证书里拿公钥 | `RsaKey::fromCertificatePem` / `EccKey::fromCertificatePem` | 自己找 SubjectPublicKeyInfo 的偏移 |
| 需要随机数 | `Random::fill` / `Random::bytes` | `std::rand`、时间戳、GUID |

## 2. 最小例子

```cpp
#include <vine/crypto/AesGcm.hpp>
#include <vine/crypto/Digest.hpp>
#include <vine/crypto/Hasher.hpp>
#include <vine/crypto/Random.hpp>

using namespace vn::crypto;

// 摘要：一次性用自由函数，分块到达用 Hasher。
const auto fingerprint = digest(Digest::kSha256, bytes);

Hasher hasher(Digest::kSha256);   // 也可以用名字：Hasher hasher(u8"sha256")
hasher << chunk1 << chunk2;       // operator<< 接 span / MemoryStream / u8string_view
const auto value = hasher.finish();

// AEAD：密钥 16 / 24 / 32 字节，IV 非空且同一密钥下不重复。
const auto key = Random::bytes(32);
const auto iv  = Random::bytes(12);
const AesGcm::Sealed sealed = AesGcm::seal(key, iv, plaintext, aad);
const auto recovered        = AesGcm::open(key, iv, sealed.ciphertext, sealed.tag, aad);
```

`aad`（附加认证数据）是"不加密但要被认证"的字节：头部、版本号、消息序号。把它当成便宜的完整性保护。

## 3. 流式对象的契约

`AesGcm` / `AesCbc` / `AesCtr` 都是同一个形状：工厂造出对象 → 多次 `update(chunk)` → 一次 `finish()`。

- **加密**：`update()` 返回的字节可以立刻写走；`finish()` 通常返回空，但 GCM 的标签要等 `finish()` 之后取
  （`tag()`）。流式加密器的 `update()` 可能返回**比输入少**的字节——CBC 要留下不满一块的尾巴，`finish()` 才补齐填充。
- **解密**：GCM 的明文在 `finish()` **成功之前不可信**（也就是标签还没校验）。惯用法是先收着，`finish()` 不抛再使用。
- `update()` 的分块大小任意，包括 0 字节；结果与怎么切分无关。
- 对象可移动、不可拷贝；`finish()` 之后再用它（`update()` / 再 `finish()`）抛 `std::logic_error`。

## 4. 密钥与文件格式

| 你要的东西 | 调用 | 产物 |
| --- | --- | --- |
| 私钥（传统 DER/PEM） | `toPrivateDer` / `toPrivatePem` | RSA：PKCS#1 `RSA PRIVATE KEY`；ECC：SEC1 `EC PRIVATE KEY` |
| 公钥（DER/PEM） | `toPublicDer` / `toPublicPem` | X.509 `SubjectPublicKeyInfo`，标签 `PUBLIC KEY` |
| 口令保护私钥 | `toEncryptedPrivateDer/Pem(password, iterations)` | PKCS#8 `ENCRYPTED PRIVATE KEY`（PBES2 + AES-256-CBC + PBKDF2） |
| 从证书取公钥 | `fromCertificateDer/Pem` | X.509 证书里的 `SubjectPublicKeyInfo` |
| ECC 裸公钥点 | `publicX963()` / `fromPublicX963()` | X9.63 未压缩点（`0x04 || X || Y`） |

- 读的方向对称：`fromPrivateDer/Pem`、`fromPublicDer/Pem`、`fromEncryptedPrivateDer/Pem`、`fromCertificateDer/Pem`。
- 两种口令保护的 PEM 都能读：现代的 `ENCRYPTED PRIVATE KEY`（PKCS#8）与旧的
  `RSA PRIVATE KEY` + `Proc-Type: 4,ENCRYPTED` / `DEK-Info:`（2020 年前的 `openssl rsa -aes128`）。写只写前者。
- 口令保护的块**不能**喂给 `fromPrivatePem`：它会明确报错并告诉你改用 `fromEncryptedPrivatePem`。
- 证书只提供公钥：本层不验证签名、不看有效期、不管 issuer 和信任链，那些属于 appfw 的策略层。
  证书里装的是 ECC 密钥时，调 `RsaKey::fromCertificatePem` 会报“不是 RSA 公钥”；证书包只读第一张。
- `iterations` 默认 200000，写在文件里，所以调高只影响新写的文件；读的一方只需要口令。
- 口令保护的私钥文件本身就是口令校验器：拿到文件的人可以离线无限次尝试。**唯一的防线是口令强度**。

## 5. 线程与所有权

- `Random`、`digest()`、`Hasher`、`Hmac`、`Hkdf`、`Pbkdf2`、`Argon2`、`AesGcm` 的**不同实例**可以并发使用。
- **一个 `RsaKey` / `EccKey` 对象只属于一个线程**：后端把每次操作的随机上下文绑进密钥对象，两个线程共用一个对象会撞车。
  要么一线程一把钥匙，要么自己加锁。
- `AesGcm` / `AesCbc` / `AesCtr` / `Hasher` / `Hmac` 是单次使用的对象：别跨线程传递正在计算的同一个实例。
- 密钥析构时由后端释放，但**不保证内容被抹掉**；不能活到进程结束的密钥属于密钥库，不属于一个值对象。

## 6. 失败长什么样

只有三种异常，靠类型就能分支：

| 异常 | 含义 | 例子 |
| --- | --- | --- |
| `std::invalid_argument` | 你给的输入不成立 | 密钥长度不对、IV 空、密文长度不是 16 的整倍数、口令错 |
| `std::logic_error` | 用法不成立 | 公钥对象上调用 `sign()`、`finish()` 之后再 `update()` |
| `std::runtime_error` | 后端拒绝了这次运算 | 标签校验失败、后端编码失败 |

有意为之的两处模糊：**口令错**与**结构损坏**抛出同一个 `invalid_argument`（不告诉攻击者是哪一种）；
GCM 标签校验失败只抛 `runtime_error`，且之前 `update()` 出来的明文必须丢弃。

## 7. 不要做的事

- 不要用 `AesCbc`/`AesCtr` 替代 `AesGcm`：前者不认证，密文可被按位篡改（CBC 还有填充预言）。
- 不要自己比较 MAC：用 `macEquals()`，它是恒定时间的。
- 不要用 `digest()` 或 `Hkdf()` 处理口令：两者都设计得很快，正好方便离线爆破。
- 不要在同一个密钥下重复 IV / 计数器：GCM 与 CTR 的保密性都会当场崩掉。
- 不要把 RSA 当块加密用：`encrypt()` 的上限是 `密钥字节数 - 66`。
- 不要自己拼加密私钥的容器：用 `toEncryptedPrivatePem()`，别的工具（如 `openssl pkcs8`）才认。
- 不要把证书当成“已经可信”：`fromCertificatePem` 只取出公钥，签名、有效期与信任链要你自己判断。

## 8. 构建与测试

```bash
cmake -S . -B build
cmake --build build --config Debug --target test_crypto
build/bin/Debug/test_crypto.exe            # Windows
```

测试用例在 [`tests/test_crypto/`](../../tests/test_crypto/)，其中口令保护私钥的两组用例带着 OpenSSL 生成的固定夹具——
证明"别人写的能读进来"，并且本模块写出的文件能用 `openssl pkcs8 -in ... -passin ...` 打开。

公开头文件在 [`src/base/crypto/sdk/vine/crypto/`](../../src/base/crypto/sdk/vine/crypto/)。
