#include <vine/ICloneable.hpp>
#include <vine/IComparable.hpp>
#include <vine/INameable.hpp>
#include <vine/IStringable.hpp>

// The one definition of every core interface's descriptor. The headers only declare desc(); a single out-of-line
// definition is what makes the modules that implement these interfaces call one function instead of each compiling its
// own copy, and it is why the interface classes carry VN_CORE_API.

VN_CORE_NS_BEGIN

VN_DEFINE_INTERFACE(ICloneable)

VN_DEFINE_INTERFACE(IComparable)

VN_DEFINE_INTERFACE(IStringable)

VN_DEFINE_INTERFACE(INamed)

VN_DEFINE_INTERFACE(INameable, INamed)

VN_CORE_NS_END
