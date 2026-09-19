/**
 * @brief The identity a GPU measurement is reported under.
 *
 * A measurement is a number in a timestamp query; what makes it a PASS' number is the lookup from the
 * measured render graph back to the session's target table, and that lookup is the only thing a host can
 * match a sample against. So it is pinned here, device-free, in the same place the rest of the backend's
 * device-independent rules are pinned:
 *
 *   * an off-screen pass is named by ITS pass name (setPassOrder's identity), its target's name and its
 *     order, which is what a host reads a report by;
 *   * the window target's graph is named "window" -- every window pass is a VIEW of that one graph, so there
 *     is no single pass behind the interval and pretending otherwise would name the wrong thing;
 *   * a graph the target no longer holds -- a pass rebuilt between the measured frame and the read -- keeps
 *     its target but reports "(rebuilt)": the interval happened, the pass behind it is not knowable any more.
 *
 * The numbers themselves need a device (see the self-test's gpu profile phase); these names do not.
 */

#include <gtest/gtest.h>

#include <string>

#include <vsg/app/RenderGraph.h>

#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/vsg/VsgGpuProfile.hpp>
#include <vine/vsg/VsgRenderTargetEntry.hpp>

using namespace vine::graphics;

namespace
{

/// A named target with one named pass rendering into a render graph of its own.
struct NamedPass
{
    vine::intrusive_ptr<RenderTarget>  target;
    vine::intrusive_ptr<RenderPass>    pass;
    ::vsg::ref_ptr<::vsg::RenderGraph> graph;
    vine::vsg::VsgRenderTargetEntry    entry;

    explicit NamedPass(const char8_t* name, int order = 0)
    {
        target = vine::intrusive_ptr<RenderTarget>(new RenderTarget());
        pass   = vine::intrusive_ptr<RenderPass>(new RenderPass());
        target->setName(vine::String(name));
        pass->setName(vine::String(name));
        graph = ::vsg::RenderGraph::create();

        auto& objects  = entry.passes[vine::vsg::SlotKey::ownerPass(pass.get())];
        objects.graph  = graph;
        objects.order  = order;
        entry.graph    = nullptr; // off-screen: the target-level graph belongs to the window target alone
    }
};

} // namespace

TEST(GpuProfileNamingTest, AnOffScreenPassIsNamedByItsPassTargetAndOrder)
{
    NamedPass named(u8"shadow", 3);
    const auto sample = vine::vsg::detail::describeGraph(named.target.get(), named.entry, named.graph.get());
    EXPECT_EQ(sample.pass, "shadow");
    EXPECT_EQ(sample.target, "shadow");
    EXPECT_EQ(sample.order, 3);
}

TEST(GpuProfileNamingTest, TheWindowGraphIsNamedWindowRatherThanAfterOneOfItsViews)
{
    // The window target is the one keyed by nullptr, and its graph carries every window pass' views.
    vine::vsg::VsgRenderTargetEntry window_entry;
    window_entry.graph = ::vsg::RenderGraph::create();
    const auto sample = vine::vsg::detail::describeGraph(nullptr, window_entry, window_entry.graph.get());
    EXPECT_EQ(sample.pass, "window");
    EXPECT_EQ(sample.target, "window");
}

TEST(GpuProfileNamingTest, AGraphTheTargetNoLongerHoldsKeepsItsTargetAndSaysSo)
{
    // A pass rebuilt since the measured frame: the interval is real, the pass behind it is gone.
    NamedPass named(u8"gbuffer", 1);
    const auto unknown = ::vsg::ref_ptr<::vsg::RenderGraph>(::vsg::RenderGraph::create());
    const auto sample  = vine::vsg::detail::describeGraph(named.target.get(), named.entry, unknown.get());
    EXPECT_EQ(sample.pass, "(rebuilt)");
    EXPECT_EQ(sample.target, "gbuffer");
}
