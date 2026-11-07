# ADR-2026-11-07 digest vocabulary

- 状态：已取代（摘要只有一套词汇的决定仍现行；类名“Calculator”与调用手法由
  `ADR-2026-11-07-incremental-api-shape.md` 取代，`DigestCalculator` 现名 `Hasher`）
- 日期：2026-11-07
- 相关：`.ai/design/crypto.md`、`ADR-2026-11-07-crypto-api-shape.md`

## 背景

`vn::crypto` 里“选一个哈希”有三种拼法：`Hash::sha256(data, size)` 这类按算法写的静态函数（10 个算法 × 2 个重载），
`HashCalculator::Algorithm::ALG_SHA256` 这个运行期枚举，以及后加的 `Hmac::Algorithm::kSha256`。
同时 `Hash` 的返回类型按算法写死（`array<uint8_t,16>` … `array<uint8_t,64>`），调用方无法写与算法无关的代码：
HMAC、RSA、ECC 只能各自把 SHA-256 写进实现里。`Hash::sha256(std::ostream&)` 还把 I/O 带进了算法头文件，
并且一次性接口内部每次都把 `(ptr, size)` 包成一个 `MemoryStream` 再读回来。

## 选项

1. **保持现状**：每个算法一个静态函数。优点是无风险；缺点是三套词汇、写不出通用代码、I/O 依赖。
2. **每种算法一个类**（`class Sha256 { update(); final(); }`，.NET / Java 风格）。
   缺点是所有类形状完全相同，多出 N 个同形类型；运行期选择还得再加基类或 variant，多一层虚调用。
3. **一套词汇 + 两种形态**：`enum class Digest`（唯一算法身份）+ 自由函数 `digest()`（一次性）+
   `DigestCalculator`（增量）；长度、名字、名字解析都是同一套函数。

## 决定

选 **3**。

- `Digest` 的枚举值是本模块自己的连续序号，与 wolfSSL 的 `WC_HASH_TYPE_*` 无关；映射只在一个函数里。
- 一次性与增量共用 `src/DigestOps.cpp` 里的一份后端上下文与分发表，实现只写一遍。
- `digest()` 返回 `std::vector<std::byte>`：长度由算法决定，调用方用 `digestSize()` 判断。
- 失败一律抛异常（`std::runtime_error` / `std::invalid_argument`），连旧的“失败返回全零数组”也改掉：
  一个全零摘要与真实摘要无法区分，这正是 G11 要避免的隐式失败。
- 删除 `Hash` / `HashCalculator` / `Hmac::Algorithm`。仓库内唯一的消费者是 `ByteSequenceFingerprint` 与测试，已同步迁移；
  `ByteSequenceFingerprint` 的公开接口不变。

## 后果

- 调用方写 `digest(Digest::kSha256, bytes)`，从名字或配置得到 `Digest` 时只需 `digestFromName`。
- 新增算法只改 `Digest` 枚举 + `DigestOps` 的四个 switch，不新增类型、不改调用方。
- 公开 API 有一次破坏性变更（`Hash` 消失）。仓库外无消费者；仓库内调用点在同一个提交里改完。
- `Hash::*（std::ostream&）` 的流式用法不再有专门入口；需要时用 `DigestCalculator` 分块喂入，
  这样 I/O 依赖与算法实现分离。

## 证据

- `tests/test_crypto` 54 例全绿（含 9 种摘要的已知向量、增量与一次性一致、名字归一化、空输入）。
- gate 结果：`check_public_docs`（baseline 已更新）、`check_include_hygiene`、`check_export_annotations` 0 报告。
- 移植时踩到的两件事已写入设计文档：SHA-3 只有按变体的入口（`P14`）、SDK 自由函数必须标 `VN_CRYPTO_API`（`P15`）。
