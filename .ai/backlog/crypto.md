# crypto 的开放待办

- 状态：开放
- 触发条件：出现真实消费者或对端格式要求时按条目开始；条目完成即删除

## 1. 工程质量

- **错误码分类**：现在统一抛 `std::runtime_error` / `std::invalid_argument`；
  需要区分“输入非法 / 后端失败 / 认证失败”时再引入错误枚举——只在调用方真的要分支时做。
  注意口令错误与结构损坏目前有意共用一个 `std::invalid_argument`（见 `crypto.md` 的 `P21`）。
- **Argon2 线程模式**：`WOLFSSL_ARGON2_THREADS` 可以让多 lane 并行；现在没开，单线程足够口令场景。
  触发：口令派生成为可感知的等待时再评估。
- **加密私钥的写出方案可配置**：现在固定 PBES2 + AES-256-CBC + PBKDF2（默认 PRF，即 HMAC-SHA1）。
  `wc_EncryptPKCS8Key_ex` 的 `hmacOid` 参数已支持换 PRF，AES-128 也只差一个 `encAlgId`；
  只在真的有对端要求时再抬到公开 API（`G8`：没有消费者的扩展点不做）。
- **一把密钥一个线程**：`RsaKey` / `EccKey` 把每次操作的随机上下文绑进密钥对象（`P18`）。
  要并发共享同一把钥匙就得每对象一把锁；目前没有消费者需要，保持“一线程一钥匙”并在文档里写明。

## 2. 要新能力才做的事

- **第二个 AEAD（ChaCha20-Poly1305）**：后端库里已经编进去（`chacha20_poly1305.c`），模块没有入口。
  触发：目标平台没有 AES 硬件加速，或需要与只支持 ChaCha 的对端互通。
- **Ed25519 / X25519**：后端可用，模块没有入口。触发同上，但形状要决策（新类 vs 让 `EccKey` 变胖）。
- **PKCS#12 密钥库**：没做调研。大概率属于 appfw 的密钥管理，而不是算法层。

## 3. 验证

- 泄漏检测：Windows 的 ASAN 不支持 `detect_leaks`（运行时报 "not supported on this platform"），
  所以 “无泄漏” 目前没有证据。触发：linux 侧接入 `scripts/asan_check.sh` 并加一个 crypto 目标。
