#include <vine/brep/Backend.hpp>

VN_BREP_NS_BEGIN

bool hasBackend() noexcept
{
#if defined(VN_BREP_HAS_OCCT)
    return true;
#else
    return false;
#endif
}

VN_BREP_NS_END
