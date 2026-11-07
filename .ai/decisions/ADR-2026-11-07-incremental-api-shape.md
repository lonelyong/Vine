# ADR-2026-11-07 incremental API shape

- 状态：已采纳
- 日期：2026-11-07
- 相关：`.ai/design/crypto.md`、`ADR-2026-11-07-digest-vocabulary.md`（摘要词汇部分仍现行，命名部分由本文取代）

## 背景

摘要与 MAC 都有两种形态：数据已在内存，或数据一块块到来。仓库原先用 `Hash`（每算法一个静态函数）
与 `HashCalculator` / `HmacCalculator` 覆盖这两种形态。合并成 `Digest` 词汇后，命名问题暴露出来：

- `Calculator` 是我们自造的词，业界没有这种叫法，读代码的人要先猜它是干什么的。
- 同一个概念两套入口：`Hash::sha256(...)`（类 + 静态）与 `HashCalculator`（类 + 实例），
  再加 `Hmac::compute` 与 `HmacCalculator`，四种拼法。
- 增量对象的方法名 `write()` / `digest()` 与一次性入口同名，`digest()` 既指“算出结果”又指“那个自由函数”。

## 选项

1. 保留 `*Calculator`。缺点：自造词、与业界不好对照。
2. 每种算法一个类（`class Sha256 { update(); final(); }`）。缺点：形状完全相同的 N 个类型；
   运行期选择还要再加基类或 variant。
3. **一次性用自由函数、增量用“东西本身”命名的对象**，方法统一 `update()` / `finish()`。
   这正是 OpenSSL（`EVP_DigestUpdate` / `EVP_DigestFinal`、`HMAC_CTX`）、libsodium
   （`crypto_hash_sha256` + `..._state` + `init/update/final`）、RustCrypto（`Hasher::update/finalize`）、
   Go（`hash.Hash::Write/Sum`）、Qt（`QCryptographicHash::addData/result`）共同采用的形态。
4. 全用静态类（`Hasher::create(Digest)`）—— 只是把状态藏进句柄，多一层间接，没有收益。

## 决定

选 **3**：

| 概念 | 一次性 | 增量对象 | 结束 |
|---|---|---|---|
| 摘要 | `digest(Digest, span)` | `Hasher`（`Hasher(Digest)` / `Hasher(name)`） | `finish()` |
| MAC | `hmac(Digest, key, span)` | `Hmac`（`Hmac(Digest, key)`） | `finish()` |
| AEAD | `AesGcm::seal` / `open` | `AesGcm::encryptor` / `decryptor` | `finish()` |

- 增量对象的输入方法统一叫 `update()`（不是 `write()`）：`write` 会让人以为在写文件，而 `update` 是
  OpenSSL / libsodium / RustCrypto 一致的动词。
- 结束方法统一叫 `finish()`，与 `AesGcm::finish()` 相同；`digest()` 这个名字留给自由函数，避免“方法名撞函数名”。
- 常量时间比较改名 `macEquals()`：它是自由函数，名字必须自己说明“比较两个 MAC”，不能挂在 `Hmac` 类上
  （该类现在是增量对象，不是命名空间）。
- `Hasher` 采用业界通用名（RustCrypto `Hasher`、Go `hash.Hash`）；不叫 `DigestContext`，因为那是 OpenSSL 的
  C 风格上下文名，读起来像后端管线而不是“正在算的东西”。

## 后果

- 破坏性改名一次完成（仓库内调用点全部在同一个提交里改完）：`DigestCalculator` → `Hasher`，
  `HmacCalculator` → `Hmac`，`Hmac::compute` → `hmac()`，`Hmac::verify` → `macEquals()`，
  `AesGcm::write` → `AesGcm::update`。一次性与增量的方法名不再重叠。
- 三个类（`Hasher` / `Hmac` / `AesGcm`）的增量形态现在是同一个手势：构造 → `update()` → `finish()`，
  调用方不需要按类型记不同动词。
- 代价：`Hmac` 这个名字从“静态工具类”变成“增量对象”，老读者会有一个短暂的认知迁移；换来的是
  与库外经验一致的命名。

## 证据

- `tests/test_crypto` 54 例全绿（含 RFC 4231 HMAC 向量、NIST GCM 向量、分块一致性、标签失败）。
- 迁移时被批量替换误伤的普通 `std::ostream::write` 调用已恢复，说明这类改名必须逐点确认而不是全局替换 ——
  已记入 `.ai/memory/`。
