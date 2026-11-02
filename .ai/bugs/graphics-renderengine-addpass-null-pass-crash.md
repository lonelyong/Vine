# RenderEngine::addPass 在空 pass 上先解引用后检查

- **状态**：待修复；现有测试只覆盖"空 pass + 空内容"与"有效 pass"，未覆盖空 pass + 非空内容。
- **发现日期**：2026-11-02
- **模块**：`src/viz/graphics`
- **涉及**：`RenderEngine::addPass(intrusive_ptr<RenderPass>, intrusive_ptr<Scene>, int)`、`RenderEngine::insertSlot`

## 现象与影响

`addPass(pass, content, order)` 在 `content != nullptr` 时先调用 `slot.pass->setContent(...)`
（`src/viz/graphics/src/RenderEngine.cpp:384`），然后就后继的注册才做空指针检查
（`insertSlot()`，`src/viz/graphics/src/EngineCompile.cpp:569`）。传入空 pass 且带内容时，`setContent` 是普通成员函数
（`RenderPass.hpp:133`，写 `desc_.content`），对空指针调用即解引用空指针：未定义行为，通常是当场崩溃。

同一缺陷还有第二个入口：`bindPassContent()` 只有找到匹配的 slot 才写内容，空 pass 是安全的——说明“空 pass 被拒绝”
本来就是这条 API 的设计意图，只是注册路径把检查放在了太后面。

## 根因

检查与使用分属两层：`insertSlot()` 负责拒绝空 pass，`addPass()` 却在它之前就已经把内容绑到了 pass 上。
顺序错了，保护也就没覆盖到。

## 修改建议

1. 在 `addPass()`（两个重载）的最前面拒绝空 pass，直接返回空指针，并且**不**做任何内容绑定。
2. 空 pass 是否报诊断：与既有约定保持一致即可（注册失败属于调用方接线错误，可按 episode 报一次
   `UnsupportedRequest`，亦可静默返回 nullptr）；两种都行，但要写进头文件契约，避免下一个人再猜。
3. 顺带核对 `addPassOwnedBy()` / `PassRegistry::add()` 的入口是否都经由同一个"先检查后使用"的门，避免第三处漏网。

## 验证建议

- `RenderEngineTest`：`addPass(nullptr, scene, 0)` 返回 `nullptr`，`passCount()` 不变，进程不崩。
- 两个重载都覆盖；再补一条"空 pass + 空内容"确认行为未变。

## 设计约束

该问题涉及 **G4**（空参数的失败语义必须写清且与实际一致）与 **G11**（一个显而易见的接线错误不应变成崩溃）。
