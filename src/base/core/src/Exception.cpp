#include <vine/Exception.hpp>

VN_CORE_NS_BEGIN

Exception::Exception(int code) noexcept
  : code_(code)
{
    switch (code) {
    case INDEX_OUT_OF_RANGE: msg_ = u8"Index was out of range."; break;
    case ARGUMENT_NULL: msg_ = u8"The parameter cannot be null."; break;
    default: break;
    }
}

VN_CORE_NS_END
