# ADR-2026-11-07 AES-GCM streaming

- 状态：已采纳
- 日期：2026-11-07
- 相关：`.ai/design/crypto.md`（`C14`、`P4`、`D2`）、`ADR-2026-11-07-crypto-api-shape.md`（其“缓冲式增量 GCM”决定由本文取代）

## 背景

第一版 `AesGcm` 的增量形态是**缓冲式**：`update()` 把输入攒进一个 vector，`finish()` 一次算完。
当时的理由是 wolfSSL 的增量 GCM 入口在不同版本签名不同，而调错的失败方式是“放行未认证数据”——代价不可接受。

代价是记在案的一条缺陷（旧 `P4`）：内存随输入线性增长，大文件等于把整个文件读进内存。

升级到 wolfSSL v5.9.4 后，后端提供了稳定且明确的流式入口：

- `wc_AesGcmEncryptInit/Update/Final`、`wc_AesGcmDecryptInit/Update/Final`；
- `wc_AesGcmDecryptFinal` 带 `WARN_UNUSED_RESULT`，调用方不检查返回值编译器会报错；
- `update()` 的输出长度恒等于输入长度（GCM 是 CTR 流），不需要保留回退块。

## 决定

改为真正的流式实现，去掉缓冲：

| 形态 | 行为 |
|---|---|
| 构造 | 初始化上下文、设密钥与 IV、一次喂完 AAD（GCM 要求所有关联数据在首字节密文之前） |
| `update(span)` | 处理这一块，返回等长的输出（加密侧密文，解密侧明文） |
| `update(span, out&)` | 同上，输出写进调用方缓冲，避免每块一次分配 |
| `finish()` | 加密侧写标签；解密侧做标签判定，失败抛 `std::runtime_error` |

## 后果

- 任意大小的输入都能常量内存处理；`seal` / `open` 也走同一条路径，测试用“分块结果 == 一次性结果”锁住一致性。
- **新的契约**（写进头文件 `@warning` 与设计文档 `C14`/`P4`）：解密时 `update()` 交出的字节在 `finish()` 成功之前
  不可信。这是流式 AEAD 的固有性质，不是实现缺陷；调用方要么把输出写到可回滚的地方，要么最后再提交。
  旧实现“先验证再给明文”的优势因此消失，换来的是有界内存——这个取舍必须明说，不能靠实现悄悄保证。
- `AesGcm` 不再提供 `operator<<`：输入不再被攒起来，流操作符“喂进去以后再取”的语义不再成立。
- `finish()` 对 GCM 返回空（GCM 没有尾部块），保留返回值是为了与 `Hasher` / `Hmac` 的手势一致。

## 证据

- `tests/test_crypto/CipherTest.cpp`：1000 字节按 1 + 509 + 余下分块加密，密文与 `seal()` 逐字节相同；
  分块解密还原原文；篡改任一密文字节或标签都在 `finish()` 抛异常；空输入仍然认证 AAD。
- NIST SP 800-38D case 2 向量继续通过，说明流式路径与一次性路径给出同样的结果。
- 启用方式：`target_compile_definitions(wolfssl PRIVATE WOLFSSL_AESGCM_STREAM)`，并在 `src/WolfSsl.hpp` 重新声明该宏
  ——生成的 `options.h` 会 `#undef` 它，命令行传入的定义活不过那一行，而 `Aes` 的结构布局取决于它。
