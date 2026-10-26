#pragma once

#include <type_traits>

#ifdef __clang__
#    define VN_CC_CLANG
#elif defined(__GNUC__)
#    define VN_CC_GNU
#elif defined(_MSC_VER)
#    define VN_CC_MSVC
#elif defined(__INTEL_COMPILER)
#    define VN_CC_INTEL
#else
#    error Unknown compiler.
#endif

#if defined(_MSC_VER) || defined(_WIN32) || defined(_WINDOWS)
#    define VN_EXPORT __declspec(dllexport)
#    define VN_IMPORT __declspec(dllimport)
#else
#    define VN_EXPORT
#    define VN_IMPORT
#endif

#if defined(__LP64__) || defined(_LP64) || defined(_WIN64) || defined(__x86_64__)
#    define VN_ARCH_64
#endif

#ifndef VN_ROOT_NS
#    define VN_ROOT_NS vine
#endif

#define VN_ROOT_NS_BEGIN                                                                                                                                        \
    namespace VN_ROOT_NS                                                                                                                                        \
    {

#define VN_ROOT_NS_END }

#define VN_DISABLE_COPY(ClassName)                                                                                                                              \
  private:                                                                                                                                                     \
    ClassName(const ClassName&)            = delete;                                                                                                           \
    ClassName& operator=(const ClassName&) = delete;

#define VN_DISABLE_MOVE(ClassName)                                                                                                                              \
  private:                                                                                                                                                     \
    ClassName(ClassName&&)            = delete;                                                                                                                \
    ClassName& operator=(ClassName&&) = delete;

#define VN_DISABLE_COPY_MOVE(ClassName)                                                                                                                         \
    VN_DISABLE_COPY(ClassName)                                                                                                                                  \
    VN_DISABLE_MOVE(ClassName)


#define VN_ENABLE_ENUM_FLAGS(Enum)                                                                                                                              \
    inline constexpr Enum& operator|=(Enum& left, Enum right)                                                                                                  \
    {                                                                                                                                                          \
        return left = static_cast<Enum>(static_cast<std::underlying_type_t<Enum>>(left) | static_cast<std::underlying_type_t<Enum>>(right));                   \
    }                                                                                                                                                          \
    inline constexpr Enum& operator&=(Enum& left, Enum right)                                                                                                  \
    {                                                                                                                                                          \
        return left = static_cast<Enum>(static_cast<std::underlying_type_t<Enum>>(left) & static_cast<std::underlying_type_t<Enum>>(right));                   \
    }                                                                                                                                                          \
    inline constexpr Enum& operator^=(Enum& left, Enum right)                                                                                                  \
    {                                                                                                                                                          \
        return left = static_cast<Enum>(static_cast<std::underlying_type_t<Enum>>(left) ^ static_cast<std::underlying_type_t<Enum>>(right));                   \
    }                                                                                                                                                          \
    inline constexpr Enum operator|(Enum left, Enum right)                                                                                                     \
    {                                                                                                                                                          \
        return static_cast<Enum>(static_cast<std::underlying_type_t<Enum>>(left) | static_cast<std::underlying_type_t<Enum>>(right));                          \
    }                                                                                                                                                          \
    inline constexpr Enum operator&(Enum left, Enum right)                                                                                                     \
    {                                                                                                                                                          \
        return static_cast<Enum>(static_cast<std::underlying_type_t<Enum>>(left) & static_cast<std::underlying_type_t<Enum>>(right));                          \
    }                                                                                                                                                          \
    inline constexpr Enum operator^(Enum left, Enum right)                                                                                                     \
    {                                                                                                                                                          \
        return static_cast<Enum>(static_cast<std::underlying_type_t<Enum>>(left) ^ static_cast<std::underlying_type_t<Enum>>(right));                          \
    }                                                                                                                                                          \
    inline constexpr bool operator!(Enum left)                                                                                                                 \
    {                                                                                                                                                          \
        return !static_cast<std::underlying_type_t<Enum>>(left);                                                                                               \
    }                                                                                                                                                          \
    inline constexpr Enum operator~(Enum left)                                                                                                                 \
    {                                                                                                                                                          \
        return static_cast<Enum>(~static_cast<std::underlying_type_t<Enum>>(left));                                                                            \
    }

VN_ROOT_NS_BEGIN
template <typename TEnum>
inline constexpr bool testFlag(TEnum a, TEnum b)
{
    return static_cast<bool>(static_cast<std::underlying_type_t<TEnum>>(a & b));
}

VN_ROOT_NS_END
