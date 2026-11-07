# ADR-2026-11-07 crypto API shape

- 状态：已采纳
- 日期：2026-11-07
- 相关：`.ai/design/crypto.md`、`.ai/backlog/crypto.md`

## 背景

`vn::crypto` 只有摘要（`Hash` / `HashCalculator`）和指纹。要继续做 HMAC、AES-GCM、随机字节，
并按这条路做 RSA / ECC，必须先定三件事：加解密对象怎么表达方向、增量形态与一次性形态是什么关系、
第三方库的全局初始化放在哪里。三件事都会固化在公开接口上，改起来要动所有调用方。

## 选项

### A. 加解密方向

1. 一个类 + `mode` 标志位（`encryptor` 与否由构造参数决定）。
2. 一个类 + 两个工厂（`encryptor()` / `decryptor()`），方向在构造时确定、不可变。
3. 两个类（`AesGcmEncryptor` / `AesGcmDecryptor`）。

### B. 增量形态

1. 只提供一次性 API，调用方自己攒缓冲。
2. 增量 API 直接把输入交给后端的增量入口（真流式）。
3. 增量 API 缓冲输入，`finish()` 时一次算完，与一次性走同一条路径。

### C. 后端初始化

1. 每个类型在第一次使用时各自 `std::call_once` 初始化后端。
2. 模块内一个 `detail::ensureLibraryInitialized()`，所有使用者调用它。
3. 要求调用方在启动时手动初始化。

## 决定

- A 选 **2**：一个类两个工厂。方向是构造期事实，不是运行期状态；`isEncrypting()` 只用于诊断。
  否决 1：允许“用加密对象解密”这种非法状态存在。否决 3：两个类共享的密钥/IV/AAD 校验要重复一遍（G1、G8）。
- B 选 **3**：缓冲后一次算完。理由是正确性优先：wolfSSL 的增量 GCM 入口在不同版本签名不同，
  调错的失败方式是**放行未认证数据**，而不是报错；缓冲式让增量与一次性走完全相同的代码路径（`R2` 因此可测）。
  代价（内存随输入增长）写进头文件 `@warning` 与设计文档 `P4`；有界内存版本作为独立条目留在 backlog，
  届时要有自己的测试（`D2`）。
- C 选 **2**：模块内唯一入口 + `std::call_once`。否决 1：多处初始化容易只在新类型上补，漏掉就有崩溃（见下）。否决 3：
  把顺序正确性推给每个调用方，违反“隐藏复杂性、不转移复杂性”（G7）。

## 后果

- 公开面：`AesGcm` 无 `mode` 参数，`decryptor` 必须给出标签；`write()` 之后只能 `finish()` 一次。
- 认证失败表现为 `std::runtime_error`，明文不出模块；调用方无法用返回值“带着未认证明文继续走”。
- 增量 GCM 的内存代价是已知缺陷，记录在设计与 backlog，不在实现里隐去。
- 后端初始化从“每个类型自己处理”变成“一处处理”：新增 RSA/ECC 时只需调用同一个函数，不会漏。

## 证据

- `wc_InitRng()` 未先 `wolfCrypt_Init()` 时，首次 `Random::fill` 在 `wc_InitRng → wc_LockMutex → EnterCriticalSection`
  上访问违例（0xc0000005，cdb 栈已确认）；`drbgStateMutex` 只由 `wc_DrbgState_MutexInit()`（即 `wolfCrypt_Init()`）初始化
  （`wolfcrypt/src/random.c`）。
- wolfSSL 头文件定义 `#define Md5 wc_Md5`、`#define Sha256 wc_Sha256` 等宏，因此本模块的算法枚举值一律 `k` 前缀；
  这不是风格偏好，而是避免宏替换破坏枚举。
- `tests/test_crypto` 42 例通过，含 HMAC RFC 4231 case 2 与 AES-GCM NIST SP 800-38D case 2 向量、分块一致性、篡改拒绝。
