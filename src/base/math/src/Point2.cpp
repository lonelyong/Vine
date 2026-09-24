#include <vine/math/Point2.hpp>

VN_MATH_NS_BEGIN

#define TMPL_PREFIX template <typename T>

 
#undef TMPL_PREFIX

template class VN_MATH_API Point2<float>;
template class VN_MATH_API Point2<double>;
template class VN_MATH_API Point2<bool>;
template class VN_MATH_API Point2<int8_t>;
template class VN_MATH_API Point2<uint8_t>;
template class VN_MATH_API Point2<int16_t>;
template class VN_MATH_API Point2<uint16_t>;
template class VN_MATH_API Point2<int32_t>;
template class VN_MATH_API Point2<uint32_t>;
template class VN_MATH_API Point2<int64_t>;
template class VN_MATH_API Point2<uint64_t>;

VN_MATH_NS_END
