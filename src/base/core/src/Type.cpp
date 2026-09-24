#include <vine/Type.hpp>

#include <algorithm>
#include <mutex>
#include <set>
#include <stdexcept>
#include <typeinfo>

#if defined(__GNUC__)
#    include <cxxabi.h>
#endif

#include <vine/Exception.hpp>

VN_CORE_NS_BEGIN

namespace
{

std::set<Type*> s_types;
std::mutex      s_types_mutex;

#if defined(_MSC_VER)
bool parse_type_info_msvc(const std::type_info& c_type, String& name, String& ns, String& full_name)
{
    auto n    = c_type.name();
    full_name = String::fromLocal8Bit(n);
    full_name = full_name.substr(6);
    auto idx  = full_name.rfind(u8"::");
    name      = full_name.substr(idx + 2);
    ns        = full_name.substr(0, idx);
    return true;
}
#endif

#if defined(__GNUC__)
// clang defines __GNUC__ as well, so this is used for both GCC and clang
bool parse_type_info_gnuc(const std::type_info& c_type, String& name, String& ns, String& full_name)
{
    int   status;
    char* demangled = abi::__cxa_demangle(c_type.name(), nullptr, nullptr, &status);

    if (status != 0)
        return false;

    full_name = reinterpret_cast<char8_t*>(demangled);
    free(demangled);

    size_t pos = full_name.rfind(u8"::");

    if (pos == -1) {
        name = full_name;
    }
    else {
        ns   = full_name.substr(0, pos);
        name = full_name.substr(pos + 2);
    }

    return true;
}
#endif

} // namespace

Type::Type(const std::type_info& c_type, const Type* parent, TypeKind kind, std::vector<const Type*> interfaces)
  : c_type_(c_type)
  , parent_(parent)
  , kind_(kind)
  , interfaces_(std::move(interfaces))
{
    if (get(c_type))
        throw Exception(Exception::ITEM_ALREADY_EXISTS);

    auto is_ok = false;

#if defined(_MSC_VER)
    is_ok = parse_type_info_msvc(c_type, name_, ns_, full_name_);
#elif defined(__GNUC__)
    is_ok = parse_type_info_gnuc(c_type, name_, ns_, full_name_);
#else
#    error "Unsupported compiler"
#endif

    if (!is_ok) {
        throw std::runtime_error("Runtime error.");
    }

    {
        std::lock_guard<std::mutex> lock(s_types_mutex);
        s_types.insert(this);
    }
}

Type::~Type()
{
    {
        std::lock_guard<std::mutex> lock(s_types_mutex);
        s_types.erase(this);
    }
}

bool Type::isSubclassOf(const Type* cls) const noexcept
{
    if (cls == nullptr)
        return false;

    auto self = this;
    do {
        if (self == cls)
            return true;
        self = self->parent_;
    }
    while (self);
    return false;
}

bool Type::implements(const Type* itf) const noexcept
{
    if (itf == nullptr || !itf->isInterface())
        return false;

    std::vector<const Type*> pending(interfaces_.begin(), interfaces_.end());
    std::vector<const Type*> visited;

    while (!pending.empty()) {
        const Type* t = pending.back();
        pending.pop_back();

        if (t == itf)
            return true;

        if (std::find(visited.begin(), visited.end(), t) != visited.end())
            continue;
        visited.push_back(t);

        pending.insert(pending.end(), t->interfaces_.begin(), t->interfaces_.end());
    }
    return false;
}

bool Type::isKindOf(const Type* type) const noexcept
{
    return isSubclassOf(type) || implements(type);
}

bool Type::operator==(const Type& right) const noexcept
{
    return c_type_ == right.c_type_;
}

bool Type::operator!=(const Type& right) const noexcept
{
    return !(*this == right);
}

Type* Type::get(const std::type_info& c_type)
{
    std::lock_guard<std::mutex> lock(s_types_mutex);
    auto                        it = std::find_if(s_types.begin(), s_types.end(), [&c_type](Type* c) { return c->c_type_ == c_type; });
    if (it == s_types.end())
        return nullptr;
    return *it;
}

Type* Type::get(const String& full_name)
{
    std::lock_guard<std::mutex> lock(s_types_mutex);
    auto                        it = std::find_if(s_types.begin(), s_types.end(), [&full_name](Type* c) { return c->full_name_ == full_name; });
    if (it == s_types.end())
        return nullptr;
    return *it;
}

VN_CORE_NS_END
