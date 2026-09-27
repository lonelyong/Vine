// DocumentTest.cpp
//
// 文档模型的第一层：身份、生命周期、当前选择。全在 core（无 Qt、无 UI、无渲染），所以一个无头宿主就够把它钉死。
// 五条语义（见 .ai/design/appfw-document-model.md）：
//   ① 类型注册：一个 id 一个类型，重名拒绝（不悄悄换工厂）；空 id / 空工厂拒绝。
//   ② 创建：注册过的 id 能建；未注册、工厂返回空、工厂返回的文档报的是别的类型 —— 一律拒；**创建不动 current**。
//   ③ 当前切换：只接受本管理器拥有的文档（nullptr = 没有）；值没变就不发事件。
//   ④ 关闭：脏文档被 canClose() 拒 ⇒ 返回 false，且集合与 current 都不动（宿主可以存了再关）；覆写 canClose()
//      的类型脏也能关（"问不问用户"是宿主的事，不是框架的）。
//   ⑤ 关闭的次序与所有权：先出集合、再发事件（事件处理函数走 documents() 看到的是当下的集合）；事件期间对象
//      **还活着**（能读它的 typeId()/title()）；close() 返回时销毁。当前文档被关 ⇒ current 为空，且
//      currentChanged(null) **先于** closed —— 宿主在 closed 里挑下一个当前文档，不会被随后那次通知覆盖。
//
// See .ai/design/appfw-document-model.md.

#include <gtest/gtest.h>

#include <functional>
#include <utility>
#include <vector>

#include <vine/appfw/Document.hpp>
#include <vine/appfw/DocumentManager.hpp>

namespace
{

/// 用例用的文档类型：dirty 可设、能数出"析构过几次"，并且能在事件里被读。
class TestDocument : public vn::appfw::Document {
  public:
    TestDocument(vn::String type_id, bool dirty, int* destroyed)
      : type_id_(std::move(type_id)), dirty_(dirty), destroyed_(destroyed)
    {
    }

    ~TestDocument() override
    {
        if (destroyed_ != nullptr) {
            ++*destroyed_;
        }
    }

    vn::String typeId() const override { return type_id_; }
    vn::String title() const override { return u8"测试文档"; }
    bool       isDirty() const override { return dirty_; }

    void setDirty(bool dirty) { dirty_ = dirty; }

  private:
    vn::String type_id_;
    bool       dirty_    = false;
    int*       destroyed_ = nullptr;
};

/// 一个"脏了也能关"的类型：只有它覆写 canClose()（基类的默认是 !isDirty()）。
class AlwaysCloseableDocument : public TestDocument {
  public:
    using TestDocument::TestDocument;

    bool canClose() const override { return true; }
};

constexpr const char8_t* kType = u8"test";

/// 打开载荷：调用方与类型之间的**私有协议**。派生 Object 只为了一件事：它的 Type 就是登记用的键
/// （和 EventBus 的订阅同一形状），框架从不看内容。代价是它不再是聚合体（有虚函数），所以用例里的值构造
/// 要么逐字段赋值、要么走下面的 makePayload()。
struct TestPayload : public vn::Object {
    VN_OBJECT_META_DECL;

    int        value = 0;
    vn::String locator; ///< 调用方带来的“地址”（真实场景里是 Vfs 的 locator 之类），空 = 没有。
};

/// 第二种载荷：用来钉“没人登记过这种载荷”。
struct OtherPayload : public vn::Object {
    VN_OBJECT_META_DECL;

    int id = 0;
};

/// 载荷也能派生：注册一种类型 = 接受它以及它的派生（和 EventBus 投递事件一个规矩）。
struct BasePayload : public vn::Object {
    VN_OBJECT_META_DECL;

    int base = 0;
};

/// 更具体的载荷：基类打开器接得住它（拿到的是 `BasePayload&`），但**更具体的登记先试**。
struct DerivedPayload : public BasePayload {
    VN_OBJECT_META_DECL;

    int derived = 0;
};

// 载荷的元数据：必须恰好一处定义（两处会让第二个 Type 构造抛 ITEM_ALREADY_EXISTS）。
VN_OBJECT_META_IMPL(TestPayload, vn::Object)
VN_OBJECT_META_IMPL(OtherPayload, vn::Object)
VN_OBJECT_META_IMPL(BasePayload, vn::Object)
VN_OBJECT_META_IMPL(DerivedPayload, BasePayload)

/// 载荷的字段赋値式工厂：派生 Object 之后它不再是聚合体，指定初始化器（.value = …）不再可用。
TestPayload makePayload(int value, vn::String locator = vn::String())
{
    TestPayload payload;
    payload.value   = value;
    payload.locator = std::move(locator);
    return payload;
}

/// 拷贝计数放在文件级可变变量上（不是载荷的成员）：挪动/返回载荷不会把计数挪走，只有真的拷一份才会加。
int s_payload_copies = 0;

/// 一个会计数的载荷：用来钉“**框架不替任何人拷贝载荷**”。
struct CountedPayload : public vn::Object {
    VN_OBJECT_META_DECL;

    int value = 0;

    CountedPayload() = default;
    explicit CountedPayload(int v) : value(v) {}

    /// 只有拷贝构造计数：框架要是偷偷留了一份，就正好在这里露出来。
    CountedPayload(const CountedPayload& other) : vn::Object(other), value(other.value) { ++s_payload_copies; }
};

VN_OBJECT_META_IMPL(CountedPayload, vn::Object)

/// 一个会记下来源、并且真的能 save() 的类型：钉住 source()/save() 这条线。
class SavingDocument : public TestDocument {
  public:
    SavingDocument(vn::String type_id, int* saves)
      : TestDocument(type_id, false, nullptr), saves_(saves)
    {
    }

    bool save() override
    {
        if (!source().valid()) {
            return false; // 没有来源：写不到任何地方。
        }
        ++*saves_;
        return true;
    }

    /// 真实类型在自己的 open 工厂里调 setSource()（protected）；用例用它模拟那一步。
    void adoptSource(vn::appfw::DocumentSource source) { setSource(std::move(source)); }

  private:
    int* saves_ = nullptr;
};

/// 一个注册项：id + 显示名 + 一个造 TestDocument 的工厂（dirty / 析构计数器可配）。
vn::appfw::DocumentTypeRegistration testTypeRegistration(bool dirty, int* destroyed, const vn::String& type_id = kType)
{
    vn::appfw::DocumentTypeRegistration registration;
    registration.type_id      = type_id;
    registration.display_name = u8"测试文档";
    const vn::String id       = type_id;
    registration.create       = [dirty, destroyed, id] { return new TestDocument(id, dirty, destroyed); };
    return registration;
}

bool registerTestType(vn::appfw::DocumentManager& manager, bool dirty, int* destroyed)
{
    return manager.registerType(testTypeRegistration(dirty, destroyed));
}

} // namespace

TEST(DocumentTest, RegisteringATypeLetsTheManagerCreateIt)
{
    vn::appfw::DocumentManager manager;
    EXPECT_FALSE(manager.isTypeRegistered(kType));
    EXPECT_TRUE(manager.types().empty());

    ASSERT_TRUE(registerTestType(manager, false, nullptr));
    EXPECT_TRUE(manager.isTypeRegistered(kType));

    const std::vector<vn::appfw::DocumentTypeInfo> types = manager.types();
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0].type_id, kType);
    EXPECT_EQ(types[0].display_name, u8"测试文档");

    int opened = 0;
    manager.opened.connect([&opened](vn::appfw::DocumentManager&, vn::appfw::DocumentEventArgs&) { ++opened; }).detach();

    vn::appfw::Document* document = manager.create(kType);
    ASSERT_NE(document, nullptr);
    EXPECT_EQ(document->typeId(), kType);
    EXPECT_EQ(opened, 1);
    EXPECT_EQ(manager.documents().size(), 1u) << "事件发在入集合之后，处理函数应该看得到它";
    EXPECT_EQ(manager.current(), nullptr) << "创建与选中是两件事：create() 不动 current";

    EXPECT_TRUE(manager.close(document));
    EXPECT_TRUE(manager.documents().empty());
}

TEST(DocumentTest, AnUnknownTypeOrAMismatchedFactoryCreatesNothing)
{
    vn::appfw::DocumentManager manager;
    EXPECT_EQ(manager.create(u8"nope"), nullptr) << "没注册过的类型建不出来";
    EXPECT_TRUE(manager.documents().empty());

    // 工厂与注册不符：注册在 kType 名下，却返回一个报别的类型的文档 ⇒ 拒（否则注册表与文档各说一套）。
    vn::appfw::DocumentTypeRegistration mismatched;
    mismatched.type_id = kType;
    mismatched.create  = [] { return new TestDocument(u8"other", false, nullptr); };
    ASSERT_TRUE(manager.registerType(mismatched));
    EXPECT_EQ(manager.create(kType), nullptr);
    EXPECT_TRUE(manager.documents().empty());
}

TEST(DocumentTest, ATypeIdCannotBeClaimedTwice)
{
    vn::appfw::DocumentManager manager;
    ASSERT_TRUE(manager.registerType(testTypeRegistration(false, nullptr)));

    vn::appfw::DocumentTypeRegistration second;
    second.type_id      = kType;
    second.display_name = u8"二";
    second.create       = [] { return new TestDocument(kType, false, nullptr); };
    EXPECT_FALSE(manager.registerType(second));

    const std::vector<vn::appfw::DocumentTypeInfo> types = manager.types();
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0].display_name, u8"测试文档") << "拒绝重名时不能把原来那一个换掉";

    vn::appfw::DocumentTypeRegistration unnamed;
    unnamed.create = [] { return new TestDocument(kType, false, nullptr); };
    EXPECT_FALSE(manager.registerType(unnamed)) << "空 id 拒绝";
}

TEST(DocumentTest, TheCurrentDocumentMustBeOneTheManagerOwns)
{
    vn::appfw::DocumentManager manager;
    ASSERT_TRUE(registerTestType(manager, false, nullptr));
    vn::appfw::Document* first  = manager.create(kType);
    vn::appfw::Document* second = manager.create(kType);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    int changes = 0;
    manager.currentChanged.connect(
        [&changes](vn::appfw::DocumentManager&, vn::appfw::DocumentEventArgs&) { ++changes; }).detach();

    EXPECT_TRUE(manager.setCurrent(first));
    EXPECT_EQ(manager.current(), first);
    EXPECT_EQ(changes, 1);

    EXPECT_TRUE(manager.setCurrent(first)) << "选中的没变：算成功";
    EXPECT_EQ(changes, 1) << "值没变就不发事件";

    EXPECT_TRUE(manager.setCurrent(nullptr));
    EXPECT_EQ(manager.current(), nullptr);
    EXPECT_EQ(changes, 2) << "nullptr 表示“没有当前文档”，是一次真变化";

    // 不是本管理器的文档：拒绝，而且当前选择一动不动。
    TestDocument stranger(kType, false, nullptr);
    EXPECT_TRUE(manager.setCurrent(second));
    EXPECT_FALSE(manager.setCurrent(&stranger));
    EXPECT_EQ(manager.current(), second);
}

TEST(DocumentTest, ClosingADirtyDocumentIsRefusedAndChangesNothing)
{
    vn::appfw::DocumentManager manager;
    int destroyed = 0;
    ASSERT_TRUE(registerTestType(manager, true, &destroyed));

    vn::appfw::Document* document = manager.create(kType);
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(manager.setCurrent(document));

    int closed = 0;
    manager.closed.connect([&closed](vn::appfw::DocumentManager&, vn::appfw::DocumentEventArgs&) { ++closed; }).detach();

    EXPECT_FALSE(manager.close(document)) << "脏文档被 canClose() 拒";
    EXPECT_EQ(manager.documents().size(), 1u) << "被拒 ⇒ 集合不动";
    EXPECT_EQ(manager.current(), document) << "被拒 ⇒ 当前选择不动";
    EXPECT_EQ(closed, 0) << "被拒 ⇒ 不发 closed";
    EXPECT_EQ(destroyed, 0) << "被拒 ⇒ 对象也不该被销毁";

    // 存过之后就能关：宿主先 save()，再 close()。
    static_cast<TestDocument*>(document)->setDirty(false);
    EXPECT_TRUE(manager.close(document));
}

TEST(DocumentTest, ATypeThatAllowsClosingWhileDirtyIsClosed)
{
    vn::appfw::DocumentManager manager;
    vn::appfw::DocumentTypeRegistration registration;
    registration.type_id = kType;
    registration.create  = [] { return new AlwaysCloseableDocument(kType, true, nullptr); };
    ASSERT_TRUE(manager.registerType(registration));

    vn::appfw::Document* document = manager.create(kType);
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(document->isDirty());
    EXPECT_TRUE(document->canClose()) << "这个类型覆写了 canClose()";
    EXPECT_TRUE(manager.close(document)) << "覆写之后脏也能关（“要不要问用户”是宿主的事）";
    EXPECT_TRUE(manager.documents().empty());
}

TEST(DocumentTest, ClosingErasesTheDocumentFirstAndDestroysItAfterTheEvent)
{
    vn::appfw::DocumentManager manager;
    int destroyed = 0;
    ASSERT_TRUE(registerTestType(manager, false, &destroyed));
    vn::appfw::Document* document = manager.create(kType);
    ASSERT_NE(document, nullptr);

    std::size_t documents_seen = 99;
    vn::String  type_seen;
    bool        same_pointer = false;
    manager.closed.connect([&](vn::appfw::DocumentManager& owner, vn::appfw::DocumentEventArgs& args) {
        documents_seen = owner.documents().size();
        same_pointer   = (args.document() == document);
        type_seen      = args.document()->typeId();
    }).detach();

    EXPECT_TRUE(manager.close(document));
    EXPECT_EQ(documents_seen, 0u) << "先出集合，再发事件（处理函数看到的是当下的集合）";
    EXPECT_TRUE(same_pointer);
    EXPECT_EQ(type_seen, kType) << "事件期间对象还活着，能读";
    EXPECT_EQ(destroyed, 1) << "close() 返回时已经销毁（没有别的拥有者）";
}

TEST(DocumentTest, ClosingTheCurrentDocumentClearsItBeforeFiringClosed)
{
    vn::appfw::DocumentManager manager;
    ASSERT_TRUE(registerTestType(manager, false, nullptr));
    vn::appfw::Document* first  = manager.create(kType);
    vn::appfw::Document* second = manager.create(kType);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(manager.setCurrent(first));

    std::vector<vn::String> order;
    manager.currentChanged.connect(
        [&order](vn::appfw::DocumentManager&, vn::appfw::DocumentEventArgs& args) {
            order.push_back(args.document() == nullptr ? u8"cleared" : u8"current");
        }).detach();
    manager.closed.connect([&](vn::appfw::DocumentManager& owner, vn::appfw::DocumentEventArgs&) {
        order.push_back(u8"closed");
        // 宿主在这里挑下一个：这一次选择不能被随后那次通知覆盖。
        EXPECT_TRUE(owner.setCurrent(second));
    }).detach();

    EXPECT_TRUE(manager.close(first));

    // 三次通知，次序就是契约：先"当前文档没有了"，再 closed，最后是宿主在 closed 里挑的那一个被宣告。
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], u8"cleared") << "currentChanged(null) 先于 closed";
    EXPECT_EQ(order[1], u8"closed");
    EXPECT_EQ(order[2], u8"current") << "closed 里挑的下一个被宣告，而不是被随后一次通知覆盖";
    EXPECT_EQ(manager.current(), second);
}

TEST(DocumentTest, ClosingSomethingTheManagerDoesNotOwnIsRefused)
{
    vn::appfw::DocumentManager manager;
    ASSERT_TRUE(registerTestType(manager, false, nullptr));

    TestDocument stranger(kType, false, nullptr);
    EXPECT_FALSE(manager.close(&stranger));
    EXPECT_FALSE(manager.close(nullptr));
}

TEST(DocumentTest, RegisteringATypeRecordsWhatAManagerPanelNeeds)
{
    vn::appfw::DocumentManager manager;

    vn::appfw::DocumentTypeRegistration registration;
    registration.type_id      = kType;
    registration.display_name = u8"测试文档";
    registration.description  = u8"给面板看的一行";
    registration.icon         = u8"<svg/>";
    registration.create       = [] { return new TestDocument(kType, false, nullptr); };
    ASSERT_TRUE(manager.registerType(registration));

    // 打开器带来的是"这个类型能从什么造出来"——这也是面板要显示的东西（谁注册的、吃什么载荷、产什么来源方案）。
    ASSERT_TRUE(manager.registerOpener<TestPayload>({
        .type_id       = kType,
        .priority      = 3,
        .open          = [](const TestPayload&) -> vn::appfw::Document* { return nullptr; },
        .source_scheme = u8"file",
    }));
    ASSERT_TRUE(manager.registerOpener<OtherPayload>({
        .type_id       = kType,
        .priority      = 1,
        .open          = [](const OtherPayload&) -> vn::appfw::Document* { return nullptr; },
        .source_scheme = u8"clipboard",
    }));

    const std::vector<vn::appfw::DocumentTypeInfo> types = manager.types();
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0].type_id, kType);
    EXPECT_EQ(types[0].display_name, u8"测试文档");
    EXPECT_EQ(types[0].description, u8"给面板看的一行");
    EXPECT_EQ(types[0].icon, u8"<svg/>");
    EXPECT_TRUE(types[0].can_create);
    EXPECT_TRUE(types[0].owner.empty()) << "独立管理器没有 Application，owner 只能是空";

    ASSERT_EQ(types[0].payload_types.size(), 2u);
    EXPECT_EQ(types[0].payload_types[0], TestPayload::desc()->fullName())
        << "载荷名来自载荷自己的元数据，不是登记时另给的名字（两者不会各说一套）";
    EXPECT_NE(types[0].payload_types[0].find(u8"TestPayload"), vn::String::npos);
    EXPECT_EQ(types[0].payload_types[1], OtherPayload::desc()->fullName()) << "按登记顺序列出";

    ASSERT_EQ(types[0].source_schemes.size(), 2u);
    EXPECT_EQ(types[0].source_schemes[0], u8"file");
    EXPECT_EQ(types[0].source_schemes[1], u8"clipboard");
}

TEST(DocumentTest, ATypeWithoutACreateFactoryCanOnlyBeOpened)
{
    vn::appfw::DocumentManager manager;

    vn::appfw::DocumentTypeRegistration registration;
    registration.type_id      = kType;
    registration.display_name = u8"只能打开的类型";
    // create 留空是**合法**的：没有"空实例"可起步的类型（只能从某个来源读出来）就是这样。
    ASSERT_TRUE(manager.registerType(registration));

    const std::vector<vn::appfw::DocumentTypeInfo> types = manager.types();
    ASSERT_EQ(types.size(), 1u);
    EXPECT_FALSE(types[0].can_create);
    EXPECT_EQ(manager.create(kType), nullptr) << "没有 create 工厂就建不出来";
    EXPECT_TRUE(manager.documents().empty());

    ASSERT_TRUE(manager.registerOpener<TestPayload>({
        .type_id = kType,
        .open    = [](const TestPayload&) -> vn::appfw::Document* { return new TestDocument(kType, false, nullptr); },
    }));
    TestPayload payload = makePayload(0);
    EXPECT_NE(manager.open(&payload), nullptr) << "但打开这条路是通的";
}

TEST(DocumentTest, AnOpenerBuildsADocumentFromAPayloadAndDecidesItsSource)
{
    vn::appfw::DocumentManager manager;
    ASSERT_TRUE(registerTestType(manager, false, nullptr));

    int saves = 0;
    ASSERT_TRUE(manager.registerOpener<TestPayload>({
        .type_id = kType,
        .open    = [&saves](const TestPayload& payload) -> vn::appfw::Document* {
            auto* document = new SavingDocument(kType, &saves);
            // 来源由**打开器**给出：只有它知道调用方带来的 locator 是什么（框架从不解释它）。
            document->adoptSource(vn::appfw::DocumentSource{ u8"file", payload.locator });
            return document;
        },
        .source_scheme = u8"file",
    }));

    int opened = 0;
    manager.opened.connect([&opened](vn::appfw::DocumentManager&, vn::appfw::DocumentEventArgs&) { ++opened; }).detach();

    TestPayload          opening_payload = makePayload(7, u8"/tmp/cell.vcell");
    vn::appfw::Document* document        = manager.open(&opening_payload);
    ASSERT_NE(document, nullptr);
    EXPECT_EQ(document->typeId(), kType);
    EXPECT_TRUE(document->source().valid());
    EXPECT_EQ(document->source().scheme, u8"file");
    EXPECT_EQ(document->source().address, u8"/tmp/cell.vcell");

    // 「自己拷」那一半：载荷事后被改掉，文档手里还是它自己拷下来的那份（不是指向载荷的指针）。
    opening_payload.locator = u8"/tmp/other.vcell";
    EXPECT_EQ(document->source().address, u8"/tmp/cell.vcell") << "文档持有自己拷下来的地址，不是载荷的指针";

    EXPECT_EQ(manager.documents().size(), 1u) << "事件发在入集合之后";
    EXPECT_EQ(opened, 1);

    EXPECT_TRUE(document->save());
    EXPECT_EQ(saves, 1) << "save() 写回它自己的来源（怎么解释是类型的事）";
}

TEST(DocumentTest, TheDefaultSaveRefusesAndASourceLessDocumentCannotBeSaved)
{
    vn::appfw::DocumentManager manager;
    ASSERT_TRUE(registerTestType(manager, false, nullptr));

    // 基类默认：不支持写回（一个"哪里都不写"的类型是合法的，框架不能假装它存过了）。
    vn::appfw::Document* plain = manager.create(kType);
    ASSERT_NE(plain, nullptr);
    EXPECT_FALSE(plain->source().valid()) << "新建的文档没有来源";
    EXPECT_FALSE(plain->save());

    // 会 save() 的类型，但这次没有来源：它自己判断写不到任何地方。
    int saves = 0;
    ASSERT_TRUE(manager.registerOpener<TestPayload>({
        .type_id = kType,
        .open    = [&saves](const TestPayload&) -> vn::appfw::Document* { return new SavingDocument(kType, &saves); },
    }));
    TestPayload          no_source_payload = makePayload(0);
    vn::appfw::Document* sourceless        = manager.open(&no_source_payload);
    ASSERT_NE(sourceless, nullptr);
    EXPECT_FALSE(sourceless->source().valid());
    EXPECT_FALSE(sourceless->save());
    EXPECT_EQ(saves, 0) << "没来源那次不该计数";
}

TEST(DocumentTest, AnOpenerForAnUnregisteredTypeIsRefused)
{
    vn::appfw::DocumentManager manager;
    EXPECT_FALSE(manager.registerOpener<TestPayload>({
        .type_id = kType,
        .open    = [](const TestPayload&) -> vn::appfw::Document* { return nullptr; },
    })) << "载荷没人能造出来，比不登记更糟：看起来能用，直到真去开一个";
    EXPECT_TRUE(manager.types().empty());
}

TEST(DocumentTest, NothingAcceptsAnUnknownPayload)
{
    vn::appfw::DocumentManager manager;
    ASSERT_TRUE(registerTestType(manager, false, nullptr));
    ASSERT_TRUE(manager.registerOpener<TestPayload>({
        .type_id = kType,
        .open    = [](const TestPayload&) -> vn::appfw::Document* { return new TestDocument(kType, false, nullptr); },
    }));

    OtherPayload other;
    EXPECT_EQ(manager.open(&other), nullptr) << "没人登记过这种载荷";
    EXPECT_TRUE(manager.documents().empty());

    TestPayload known = makePayload(0);
    EXPECT_NE(manager.open(&known), nullptr);
    EXPECT_EQ(manager.documents().size(), 1u);
}

TEST(DocumentTest, TheFirstAcceptingOpenerWinsByScoreThenPriorityThenOrder)
{
    vn::appfw::DocumentManager manager;
    ASSERT_TRUE(registerTestType(manager, false, nullptr));

    std::vector<std::string> tried;
    const auto               open = [](const TestPayload& payload) -> vn::appfw::Document* {
        return payload.value > 10 ? new TestDocument(kType, false, nullptr) : nullptr;
    };

    ASSERT_TRUE(manager.registerOpener<TestPayload>({
        .type_id  = kType,
        .priority = 1,
        .open     = [&tried, open](const TestPayload& payload) { tried.push_back("low"); return open(payload); },
    }));
    ASSERT_TRUE(manager.registerOpener<TestPayload>({
        .type_id  = kType,
        .priority = 5,
        .open     = [&tried, open](const TestPayload& payload) { tried.push_back("high"); return open(payload); },
    }));
    ASSERT_TRUE(manager.registerOpener<TestPayload>({
        .type_id  = kType,
        .priority = 9,
        .open     = [&tried, open](const TestPayload& payload) { tried.push_back("refined"); return open(payload); },
        .refine   = [](const TestPayload& payload) { return payload.value > 10 ? 0 : 99; },
    }));

    // value = 1：会细判那个给 99 ⇒ 先试；它拒（返回 nullptr）⇒ 再按 priority 试高优先、再低优先 ⇒ 都拒 ⇒ nullptr。
    // 顺序本身就是契约：score > priority > 登记顺序。
    TestPayload low_value = makePayload(1);
    EXPECT_EQ(manager.open(&low_value), nullptr);
    EXPECT_EQ(tried, (std::vector<std::string>{ "refined", "high", "low" }));

    // value = 20：会细判那个说"我不接"（score 0）⇒ 只剩高优先先试，成功就不再往下试。
    tried.clear();
    TestPayload high_value = makePayload(20);
    EXPECT_NE(manager.open(&high_value), nullptr);
    EXPECT_EQ(tried, (std::vector<std::string>{ "high" }));
}

// 登记里没有 open 回调：什么也造不出来 ⇒ 拒。这条检查必须在包装**之前**做 —— 包装过的 lambda 永远不是空的，
// 交给 addOpener() 去看 NULL 就永远看不见了。
TEST(DocumentTest, ARegistrationWithoutAnOpenCallbackIsRefused)
{
    vn::appfw::DocumentManager manager;
    ASSERT_TRUE(registerTestType(manager, false, nullptr));

    EXPECT_FALSE(manager.registerOpener<TestPayload>({ .type_id = kType })) << "没有 open 就没有“能造出什么”";
    EXPECT_TRUE(manager.types()[0].payload_types.empty()) << "被拒的登记不该出现在面板里";
}

TEST(DocumentTest, ANewDocumentHasNoSourceAndSourcesCompareByValue)
{
    vn::appfw::DocumentManager manager;
    ASSERT_TRUE(registerTestType(manager, false, nullptr));

    vn::appfw::Document* document = manager.create(kType);
    ASSERT_NE(document, nullptr);
    EXPECT_FALSE(document->source().valid()) << "新建的文档没有来源（不是来源为空字符串那种意思）";

    const vn::appfw::DocumentSource none;
    const vn::appfw::DocumentSource file{ u8"file", u8"/tmp/a.vcell" };
    EXPECT_FALSE(none.valid());
    EXPECT_TRUE(file.valid());
    EXPECT_TRUE(file == (vn::appfw::DocumentSource{ u8"file", u8"/tmp/a.vcell" }));
    EXPECT_FALSE(file == (vn::appfw::DocumentSource{ u8"file", u8"/tmp/b.vcell" }));
    EXPECT_FALSE(file == (vn::appfw::DocumentSource{ u8"package", u8"/tmp/a.vcell" }));
    EXPECT_FALSE(file == none) << "宿主靠这个判断“同一个来源开了两个文档吗”";
}

// 处理函数在 opened 里就把文档关了（“打开之后发现不对”）：契约是“要么给一个能用的文档，要么给空”，
// 绝不能把已经销毁的对象交回去。
TEST(DocumentTest, ADocumentClosedByItsOpenedHandlerIsNotHandedBack)
{
    vn::appfw::DocumentManager manager;
    int destroyed = 0;
    ASSERT_TRUE(registerTestType(manager, false, &destroyed));

    manager.opened.connect([&destroyed](vn::appfw::DocumentManager& owner, vn::appfw::DocumentEventArgs& args) {
        EXPECT_TRUE(owner.close(args.document()));
    }).detach();

    EXPECT_EQ(manager.create(kType), nullptr) << "文档在 opened 里被关掉 ⇒ 不能把已销毁的对象交回去";
    EXPECT_TRUE(manager.documents().empty());
    EXPECT_EQ(destroyed, 1);
}

// 匹配期间打开器表被撑大（某个 refine 里登记了新打开器）：正在被调用的那个 std::function 必须还活着，
// 而且这一次 open 只考虑它开始时就已经登记好的打开器（同 EventBus::publish 的计划快照）。
TEST(DocumentTest, AnOpenerRegisteredWhileMatchingDoesNotJoinThatCall)
{
    vn::appfw::DocumentManager manager;
    ASSERT_TRUE(registerTestType(manager, false, nullptr));

    int registered_late = 0;
    const auto open_document = [](const TestPayload&) -> vn::appfw::Document* {
        return new TestDocument(kType, false, nullptr);
    };

    ASSERT_TRUE(manager.registerOpener<TestPayload>({
        .type_id       = kType,
        .priority      = 1,
        .open          = open_document,
        .source_scheme = u8"file",
        .refine        = [&](const TestPayload&) {
            // 这个 refine 还在跑的时候就把表撑大：扩容会搬走正在执行的那个 std::function。
            if (registered_late == 0) {
                ++registered_late;
                EXPECT_TRUE(manager.registerOpener<TestPayload>({ .type_id = kType,
                                                                  .priority = 5,
                                                                  .open     = open_document }));
            }
            return 0; // 我不接这个载荷
        },
    }));

    // 第一个拒了，而刚登记的那个不参与这一次 ⇒ 这一次没有任何打开器。
    TestPayload opening = makePayload(1);
    EXPECT_EQ(manager.open(&opening), nullptr) << "匹配期间登记的打开器不参与这一次";

    // 登记是活的：下一次就轮到它了。
    EXPECT_NE(manager.open(&opening), nullptr) << "下一次 open 就该看得到它";
    EXPECT_EQ(registered_late, 1);
}

// 注册基类 = 接受派生：打开器拿到的是它登记的那个类型，但对象其实就是派生那个（不是切片出来的副本）。
TEST(DocumentTest, AnOpenerRegisteredForABasePayloadReceivesADerivedOne)
{
    vn::appfw::DocumentManager manager;
    ASSERT_TRUE(registerTestType(manager, false, nullptr));

    int  seen_base     = -1;
    bool still_derived = false;
    ASSERT_TRUE(manager.registerOpener<BasePayload>({
        .type_id = kType,
        .open    = [&](const BasePayload& payload) -> vn::appfw::Document* {
            seen_base     = payload.base;
            still_derived = obj_cast<DerivedPayload>(&payload) != nullptr;
            return new TestDocument(kType, false, nullptr);
        },
    }));

    DerivedPayload derived;
    derived.base    = 3;
    derived.derived = 4;
    EXPECT_NE(manager.open(&derived), nullptr) << "注册基类就该接住派生载荷"
                                               << "（否则 wrapper 里的 obj_cast 能过、匹配却先把人挡了）";
    EXPECT_EQ(seen_base, 3);
    EXPECT_TRUE(still_derived) << "进来的是同一个对象，只是以基类引用看它（没有切片拷贝）";
}

// 多个登记都能接：**更具体的先试**（类型说的是“这类载荷归我”），分数/优先级只在同层之间排序；
// 具体的拒了/返回空，才回落到基类那个——这就是“基类登记当兜底”的用法。
TEST(DocumentTest, TheMostSpecificPayloadTypeIsTriedFirst)
{
    vn::appfw::DocumentManager manager;
    ASSERT_TRUE(registerTestType(manager, false, nullptr));

    std::vector<std::string> tried;
    // 基类：分数与优先级都更高（99 / +9），但它在谱系上更远，所以必须后试。
    ASSERT_TRUE(manager.registerOpener<BasePayload>({
        .type_id  = kType,
        .priority = 9,
        .open     = [&tried](const BasePayload&) -> vn::appfw::Document* {
            tried.push_back("base");
            return new TestDocument(kType, false, nullptr);
        },
        .refine = [](const BasePayload&) { return 99; },
    }));
    // 派生：优先级是负的，而且遇上 derived == 0 就拒（"这个具体种类我不接"）。
    ASSERT_TRUE(manager.registerOpener<DerivedPayload>({
        .type_id  = kType,
        .priority = -5,
        .open     = [&tried](const DerivedPayload& payload) -> vn::appfw::Document* {
            tried.push_back("derived");
            return payload.derived == 0 ? nullptr : new TestDocument(kType, false, nullptr);
        },
    }));

    DerivedPayload specific;
    specific.derived = 1;
    EXPECT_NE(manager.open(&specific), nullptr);
    EXPECT_EQ(tried, (std::vector<std::string>{ "derived" }))
        << "具体者先试，成功就不再往下试（分数 99 也抢不走）";

    tried.clear();
    DerivedPayload refused;
    EXPECT_NE(manager.open(&refused), nullptr) << "具体的拒了 ⇒ 回落到基类那个（兜底）";
    EXPECT_EQ(tried, (std::vector<std::string>{ "derived", "base" }));
}

// 框架**不处理拷贝逻辑**：“自己拷”归派生类型（把需要的字段拷进文档），“提前拷”归业务（调用方在调 open 之前把要
// 留住的东西留住）。这条钉住框架那一半：从调用方手里到打开器拿到的 `const Payload&`，**一次载荷拷贝都不该发生**。
TEST(DocumentTest, TheFrameworkDoesNotCopyAPayload)
{
    vn::appfw::DocumentManager manager;
    ASSERT_TRUE(registerTestType(manager, false, nullptr));

    int value_seen = -1;
    ASSERT_TRUE(manager.registerOpener<CountedPayload>({
        .type_id = kType,
        .open    = [&value_seen](const CountedPayload& payload) -> vn::appfw::Document* {
            value_seen = payload.value;
            return new TestDocument(kType, false, nullptr);
        },
    }));

    CountedPayload counted(5);
    s_payload_copies = 0; // 从这一刻开始数：只数 open() 这一路上有没有人拷贝它
    ASSERT_NE(manager.open(&counted), nullptr);
    EXPECT_EQ(s_payload_copies, 0) << "框架不拷载荷（要留就由类型或调用方自己拷）";
    EXPECT_EQ(value_seen, 5) << "打开器看到的是同一个对象";
}
