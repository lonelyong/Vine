#include <gtest/gtest.h>

#include <vector>

#include <vine/String.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>

#include "gui/BackendChoice.hpp"

namespace
{

using vn::graphics::RenderBackendInfo;
using vn::graphics::RenderBackendRegistry;

/// One registry entry that answers @p name. No factory: this rule reads the NAMES only, and a case here never creates a
/// backend - which is what lets the whole rule be decided without a registry, a window or a GPU.
RenderBackendRegistry::Entry entry(const char8_t* name)
{
    RenderBackendInfo info;
    info.name = name;
    return RenderBackendRegistry::Entry{ info, nullptr };
}

} // namespace

// The application's name wins, whatever the alphabet says: with both backends installed, naming "vsg" brings up "vsg"
// even though "vk" sorts first.
TEST(BackendChoiceTest, TheNameTheApplicationStatedWins)
{
    const std::vector<RenderBackendRegistry::Entry> entries{ entry(u8"vk"), entry(u8"vsg") };

    const auto choice = vn::appfw::gui::chooseBackend(entries, u8"vsg");
    EXPECT_TRUE(choice.refusal.empty());
    EXPECT_EQ(choice.name, u8"vsg");
}

// Several registered and nobody named one: refused, and the refusal names every candidate. This is the case the old
// rule answered with `entries().front()` - a default picked by the backend's name, so a plugin named "aa" would have
// switched every window's renderer without a line changing.
TEST(BackendChoiceTest, SeveralRegisteredAndNoneNamedIsRefusedAndNamesThem)
{
    const std::vector<RenderBackendRegistry::Entry> entries{ entry(u8"aa_stub"), entry(u8"zz_stub") };

    const auto choice = vn::appfw::gui::chooseBackend(entries, {});
    EXPECT_TRUE(choice.name.empty()) << "an order of names is not a decision";
    EXPECT_NE(choice.refusal.find(u8"aa_stub"), vn::String::npos);
    EXPECT_NE(choice.refusal.find(u8"zz_stub"), vn::String::npos);
    EXPECT_NE(choice.refusal.find(u8"RenderControl::setDefaultBackendName"), vn::String::npos) << "the refusal says how to fix it";
}

// One registered backend and no name states nothing ambiguous: there is nothing to choose between, so the attach takes
// it (this is what keeps a single-backend host from having to name its only backend).
TEST(BackendChoiceTest, OneRegisteredWithoutANameIsTaken)
{
    const std::vector<RenderBackendRegistry::Entry> entries{ entry(u8"vsg") };

    const auto choice = vn::appfw::gui::chooseBackend(entries, {});
    EXPECT_TRUE(choice.refusal.empty());
    EXPECT_EQ(choice.name, u8"vsg");
}

// Nothing registered and nothing named: the reason a host shows is the old sentence, word for word.
TEST(BackendChoiceTest, NothingRegisteredWithoutANameIsRefused)
{
    const std::vector<RenderBackendRegistry::Entry> entries;

    const auto choice = vn::appfw::gui::chooseBackend(entries, {});
    EXPECT_TRUE(choice.name.empty());
    EXPECT_EQ(choice.refusal, u8"no render backend is registered");
}

// A stated name that is NOT registered is refused, not replaced by whatever is registered: falling back would turn a
// misspelt name into a silent switch of renderer.
TEST(BackendChoiceTest, AStatedNameThatIsNotRegisteredIsRefusedAndSaysWhatIs)
{
    const std::vector<RenderBackendRegistry::Entry> entries{ entry(u8"vk") };

    const auto choice = vn::appfw::gui::chooseBackend(entries, u8"nope");
    EXPECT_TRUE(choice.name.empty()) << "a typo must not pick another backend";
    EXPECT_NE(choice.refusal.find(u8"nope"), vn::String::npos);
    EXPECT_NE(choice.refusal.find(u8"vk"), vn::String::npos);
}
