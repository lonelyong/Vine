#include <vine/brep/Tessellation.hpp>

VN_BREP_NS_BEGIN

bool tessellationSupported() noexcept
{
    // No backend is built into this module yet; the OpenCASCADE branch lands behind VN_BREP_HAS_OCCT and this
    // becomes true in builds that carry it.
    return false;
}

vn::intrusive_ptr<vn::geometry::TriangleMesh>
tessellate(const vn::geometry::BrepShape& shape, const TessellationOptions& options)
{
    (void)shape;
    (void)options;

    // Nothing is produced until the backend branch exists. The contract in the header names the two causes of a
    // null mesh; tessellationSupported() tells a build-level cause from an input-level one.
    return {};
}

VN_BREP_NS_END
