# ADR-2026-11-07-certificates-and-legacy-keys

- 状态：已接受
- 日期：2026-11-07
- 范围：`vn::Crypto` 的证书入口、旧式加密 PEM、以及 wolfSSL 的 MD5 构建开关

## 背景

crypto 模块收口时剩下三件事，它们看起来都是"再加一点功能"，但决定的分量不同：

1. **证书**：`RsaKey::fromRawPublic` 的注释一直写着"这是证书或 JWK 解析后交过来的形状"，也就是证书解析这一步
   从来没有存在过。要不要做、做到哪一层，需要一次明确的划线。
2. **旧式加密 PEM**：`openssl rsa -aes128` 在 PKCS#8 之前写的是
   `BEGIN RSA PRIVATE KEY` + 体内的 `Proc-Type: 4,ENCRYPTED` / `DEK-Info:` 头。本模块把这种文件当"非 base64"拒绝，
   报错指向错误的方向。
3. **MD5**：`Digest::kMd5` 是公开枚举的一部分，但 wolfSSL 的 `WOLFSSL_MD5` 默认是 **disabled**，
   本构建只是碰巧被某个预设打开。换一个配置就编不过，且失败点离原因很远（`DigestOps.cpp` 里 `wc_Md5` 未声明）。

## 决定

### 1. 证书：只取公钥，不做信任判断

`RsaKey` / `EccKey` 各加两个工厂：`fromCertificateDer` / `fromCertificatePem`。实现是一行委托：
后端的 `wc_GetSubjectPubKeyInfoDerFromCert` 取出 `SubjectPublicKeyInfo`，已有的公钥导入器收下它。

明确的非目标：签名校验、有效期、issuer、SAN、信任链。这些是**策略**，需要知道"信任谁"和"现在几点"，
而本层只回答"这个算法算出了什么"。另外只读证书包里的**第一张**——从链里挑一张是调用方的决定。

### 2. 旧式加密 PEM：读，但不写

打开 `WOLFSSL_ENCKEYS`（宏 `WOLFSSL_ENCRYPTED_KEYS`），读的方向走 `wc_KeyPemToDer(pem, sz, out, sz, pass)`；
识别靠"体内有 `Proc-Type` 头"而不是标签，因为这种形式的标签仍然是 `RSA PRIVATE KEY`，标签在说谎。

不变的立场：**写出只用 PKCS#8**（见 `ADR-2026-11-07-encrypted-private-keys.md`）。旧形式是历史包袱，
支持它是为了让老文件能读进来，不是为了让新文件变成老样子。

### 3. MD5：显式打开

`set(WOLFSSL_MD5 yes CACHE STRING "" FORCE)`。模块的公开枚举里既然有 `Digest::kMd5`，
就不能让它的可用性依赖 wolfSSL 某个预设的副作用。顺带说，旧式 PEM 的密钥派生也用 MD5。

## 否决的选项

- **把证书整个包成 `X509Certificate` 类**（subject / issuer / 有效期 / 序列号 / `isValidAt()`）：本层没有消费者会
  读这些字段，而"证书有效性"是策略判断。`G8`：没有消费者的抽象不做。等 appfw 真的需要时，那是一个新模块的事。
- **证书解析自己写 DER 走位**：后端已经有 `wc_GetSubjectPubKeyInfoDerFromCert`，自己走一遍只是多一个会偏离标准的
  解析器（`G5`：预期变化应留在模块内，但"重写已有实现"不属于变化）。
- **一次返回 `std::variant<RsaKey, EccKey>`**：把"我到底期待哪种密钥"这个调用方本来就有的知识塞进返回值，
  还得让调用方再 `visit`。两个静态工厂 + 类型不符的报错更直白。
- **旧式 PEM 写出**：会让新文件退回到没有 KDF 参数的 AES-128-CBC + MD5 派生，没有任何理由。
- **不打开 `WOLFSSL_ENCKEYS`，自己解析 `DEK-Info` 并调 `wc_DecryptContent`**：同样的活，多一层容易写错的胶水。

## 后果

- `wc_KeyPemToDer` 要的是 NUL 结尾的口令，而我们的 API 传 `std::u8string_view`：实现里复制一份、用完不清零
  （口令是调用方的，本层不持有；复制体随栈回收）。
- 旧式形式的错口令与"构建不支持的算法"返回同一个错误码，报错文案因此必须同时覆盖两者（`P23`）。
- 证书导入的报错来自公钥导入器：证书里装的是 ECC 密钥而调用 `RsaKey::fromCertificatePem` 时，报的是
  "不是 RSA 公钥"（`N12`）。
- 变异测试（`T15`）顺带测出一个值得记住的事实：口令保护的私钥文件里约四分之一的单字节变异仍会被接受，
  因为 PBES2 是 CBC、没有 MAC。这不是缺陷，是这类容器的性质（`P24`）。
