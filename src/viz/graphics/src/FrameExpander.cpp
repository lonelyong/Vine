#include "FrameExpander.hpp"

#include "FrameExpand.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

FrameExpander::FrameExpander(PublicationStore& publications) noexcept
  : publications_(publications)
{}

FrameExpander::~FrameExpander() = default;

void FrameExpander::runPass(PassDraw& pass)
{
    // BIND: a draw that names a CONTENT is re-pointed at that content's PUBLISHED SNAPSHOT, so the expansion below
    // stops asking the content - which is what makes the expand safe off the content's thread. A source the store
    // does not publish is left alone and counted: correct on the calling thread, a cross-thread read anywhere else,
    // which is exactly what the count is for.
    for (DrawDescription& draw : pass.draws) {
        if (draw.content == nullptr) {
            continue;
        }
        SnapshotSource* source = publications_.sourceOf(*draw.content);
        if (source == nullptr) {
            ++unbound_;
            continue;
        }
        draw.content = source;
    }
    expandPass(pass);
}

void FrameExpander::run(FrameDescription& frame)
{
    for (FrameDescription::Pass& entry : frame.passes) {
        if (entry.draw.has_value()) {
            runPass(*entry.draw);
        }
    }
}

std::uint64_t FrameExpander::unbound() const noexcept
{
    return unbound_;
}

} // namespace detail

VN_GRAPHICS_NS_END
