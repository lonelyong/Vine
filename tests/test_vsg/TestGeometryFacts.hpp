#pragma once

#include <vector>

#include <vine/graphics/Geometry.hpp>
#include <vine/backend_vsg/internal/GeometryFacts.hpp>

/**
 * @brief Describes a LIVE geometry, the way a content-side caller does, for tests that never cross a thread.
 *
 * WHY THIS EXISTS HERE AND NOT IN THE BACKEND. The backend's fact builder takes a `GeometryDescription` - the
 * immutable value a frame carries - because the recording path must not read content (see
 * `.ai/design/graphics-threading.md`). Most tests, however, build a scene out of `Geometry` objects and then
 * describe it directly; that is legal for them because they run on the thread that owns the objects. This overload
 * is that sentence stated once, so a test says "describe this geometry" instead of spelling the conversion at every
 * call site - and the production signature stays value-only.
 *
 * @param geometry   Live geometry to describe (its current revision is what gets described).
 * @param out        Receives the entry (its spans point into @p channels / @p primitives).
 * @param channels   Caller-owned channel storage the entry borrows; cleared first, and must outlive the entry.
 * @param primitives Caller-owned primitive storage, same rule.
 * @return None when the entry was built; Unknown / Malformed as `vn::backend_vsg::buildGeometryFacts` states.
 */
inline vn::backend_vsg::FactMiss buildGeometryFacts(const vn::graphics::Geometry&        geometry,
                                            vn::backend_vsg::GeometryFacts&              out,
                                            std::vector<vn::backend_vsg::ChannelFacts>&   channels,
                                            std::vector<vn::backend_vsg::PrimitiveFacts>& primitives)
{
    const vn::backend_vsg::FactMiss miss = vn::backend_vsg::buildGeometryFacts(*geometry.description(), out, channels, primitives);
    // The ref these tests will name from their commands: the geometry's own id, which is what a hand-built
    // `RenderCommand` / `CompiledCommand` carries (a draw list's row identity is the same node id), and the revision
    // its shape is at. The backend's builder stamps no identity at all, because a description knows its revision,
    // not who published it.
    out.geometry = vn::graphics::contentRefOf(geometry.id(), geometry.revision());
    return miss;
}
