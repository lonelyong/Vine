#include "core_global.hpp"

#include <iosfwd>

VN_CORE_NS_BEGIN

class String;

VN_CORE_API std::ostream& operator<<(std::ostream& cout, const String& str);

VN_CORE_NS_END
