#include <vine/brep/Tessellation.hpp>

VN_BREP_NS_BEGIN

vn::io::Result<vn::intrusive_ptr<vn::geometry::TriangleMesh>>
tessellate(const vn::geometry::BrepShape& shape, const TessellationOptions& options)
{
    (void)options;

    if (!shape.isValid()) {
        return vn::io::IoError::InvalidData;
    }

    // No backend is built into this module yet: the contract is fixed, the OpenCASCADE body lands behind
    // VN_BREP_HAS_OCCT (see .ai/backlog/brep.md). A caller writes one branch against the failure class either way.
    return vn::io::IoError::Unsupported;
}

VN_BREP_NS_END
