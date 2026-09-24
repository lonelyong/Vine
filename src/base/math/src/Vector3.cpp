#include <vine/math/Vector3.hpp>

VN_MATH_NS_BEGIN

#define TMPL_PREFIX template <typename T>

#undef TMPL_PREFIX

template class VN_MATH_API Vector3<float>;
template class VN_MATH_API Vector3<double>;
template class VN_MATH_API Vector3<bool>;
template class VN_MATH_API Vector3<int8_t>;
template class VN_MATH_API Vector3<uint8_t>;
template class VN_MATH_API Vector3<int16_t>;
template class VN_MATH_API Vector3<uint16_t>;
template class VN_MATH_API Vector3<int32_t>;
template class VN_MATH_API Vector3<uint32_t>;
template class VN_MATH_API Vector3<int64_t>;
template class VN_MATH_API Vector3<uint64_t>;

VN_MATH_NS_END
