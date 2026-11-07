# ADR-2026-11-07-encrypted-private-keys

- 状态：已接受
- 日期：2026-11-07
- 范围：`vn::Crypto` 的 `RsaKey` / `EccKey` 私钥落盘格式

## 背景

在此之前，私钥只能导出成未加密的 PKCS#1 `RSA PRIVATE KEY` 或 SEC1 `EC PRIVATE KEY`。传统 DER 里没有任何放口令的位置：
它就是那些数字本身。要做到"私钥文件加口令"，只有两条路：

1. 给传统 DER 外面套一个加密容器（PKCS#8 `EncryptedPrivateKeyInfo`）；
2. 自己发明一个容器（比如模块自定义的头 + AES-GCM）。

方案 2 的产物没有第二个程序能读，等于把互操作性换成一点点便利，与 `G4`（接口语义明确）和 `G9`（可演进）都抵触。

## 决定

**采用 PKCS#8 + PBES2。** 具体参数：

- 外层结构：PKCS#8 v1 `EncryptedPrivateKeyInfo`；PEM 标签 `ENCRYPTED PRIVATE KEY`。
- 密钥派生：PBES2（RFC 8018）里的 PBKDF2，随机 8 字节盐，迭代次数由调用方给（默认 200000）。
- 加密：AES-256-CBC（PKCS#7 填充），随机 16 字节 IV。
- 输入接受 RSA 与 ECC 两种传统 DER：算法与曲线标识由后端从密钥本身取出。
- 读取方向接受 PBES2 以及更老的 PBES1 / PKCS#12 口令方案，因为这些参数都在结构里。

API 形状（`RsaKey` 与 `EccKey` 各四个方法，都是一个委托调用）：

| 方法 | 说明 |
|---|---|
| `toEncryptedPrivateDer(password, iterations = 200000)` | 写出 `EncryptedPrivateKeyInfo` DER |
| `toEncryptedPrivatePem(password, iterations = 200000)` | 同上，套 PEM 甲 |
| `fromEncryptedPrivateDer(der, password)` | 解出口令保护 DER 里的私钥 |
| `fromEncryptedPrivatePem(pem, password)` | 同上，先剥 PEM 甲 |

实现只有一份：`detail::encryptPkcs8Der` / `detail::decryptPkcs8Der`（`src/Pkcs8.cpp`），两类密钥共用。

## 否决的选项

- **自造容器（头 + AES-GCM + 现成的 `Argon2`）**：安全上更强（AEAD + 内存硬 KDF），但产物没人认识。否决理由：
  私钥文件的全部价值在于"别的工具也能拿它干活"，用 `openssl pkcs8` 打不开就是死路。模块已经有 `AesGcm` 与 `Argon2`，
  需要的是**存储口令凭据**而不是密钥文件时用它们，不要把两个用途混成一个格式。
- **PBES1（PBKDF2 之外的旧方案）作为写出格式**：默认迭代与摘要都太弱。仅保留为**读取**能力，兼容老文件。
- **把 `iterations` 固定成常数**：不同机器与不同威胁模型需要不同的单价，且参数随文件走，固定只会让所有产物落在同一个折中点。
- **在 `toPrivatePem()` 上加重载 `toPrivatePem(password)`**：同名重载会藏起"格式从传统 DER 换成了 PKCS#8"这个事实
  （`G4`：名字要说清语义）。改成 `toEncryptedPrivatePem`，形容词直接写出格式变化。

## 后果

- 新增一个内部文件 `src/Pkcs8.cpp`，依赖 wolfSSL 的 `wc_CreateEncryptedPKCS8Key` / `wc_DecryptPKCS8Key` /
  `wc_GetPkcs8TraditionalOffset`（`HAVE_PKCS8`，本构建默认开启，因此没有新增 CMake 选项）。
- 写出路径需要一个 RNG（生成盐与 IV），沿用模块内已有的 `detail::RandomHandle`。
- 解密是**原地**的：`wc_DecryptPKCS8Key` 把明文写在密文上，所以内部必须复制一份输入，并在返回前 `wc_ForceZero` 抹掉。
- 口令保护的文件是离线的口令校验器：拿到文件就能无限次尝试。文档与头文件都写明这一点（`.ai/design/crypto.md` 的 `P21`）。
- 测试用 OpenSSL 生成的固定夹具证明"别人写的能读"，并用 `openssl pkey -passin` 读取本模块写出的文件证明"我们写的别人能读"。
