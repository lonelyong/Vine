#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_CORE_LIB
#    define VN_CORE_API VN_EXPORT
#else
#    define VN_CORE_API VN_IMPORT
#endif

#define VN_CORE_NS_BEGIN VN_ROOT_NS_BEGIN

#define VN_CORE_NS_END VN_ROOT_NS_END

#define VN_DECLARE_PIMPL(ClassName)                                                                                                                             \
    class ClassName;                                                                                                                                           \
    class ClassName##Private;

#define VN_DECLARE_DPTR(ClassName)                                                                                                                              \
  protected:                                                                                                                                                   \
    ClassName##Private* const d_ptr;

#define VN_DECLARE_VPTR(ClassName)                                                                                                                              \
  protected:                                                                                                                                                   \
    ClassName* v_ptr;

#define VN_DECLARE_PRIVATE(ClassName)                                                                                                                           \
  public:                                                                                                                                                      \
    friend class ClassName##Private;                                                                                                                           \
    inline ClassName##Private* getDPtr()                                                                                                                       \
    {                                                                                                                                                          \
        return reinterpret_cast<ClassName##Private*>(d_ptr);                                                                                                   \
    }                                                                                                                                                          \
    inline const ClassName##Private* getDPtr() const                                                                                                           \
    {                                                                                                                                                          \
        return reinterpret_cast<const ClassName##Private*>(d_ptr);                                                                                             \
    }

// #define VN_DECLARE_CTOR_PRIVATE(ClassName)                                                                             \
//   public:                                                                                                              \
//     ClassName##Private(ClassName* vptr);

// #define VN_DECLARE_DTOR_PUBLIC(ClassName)                                                                              \
//   public:                                                                                                              \
//     virtual ~ClassName();

#define VN_DECLARE_PUBLIC(ClassName)                                                                                                                            \
  public:                                                                                                                                                      \
    friend class ClassName;                                                                                                                                    \
    inline ClassName* getVPtr()                                                                                                                                \
    {                                                                                                                                                          \
        return reinterpret_cast<ClassName*>(v_ptr);                                                                                                            \
    }                                                                                                                                                          \
    inline const ClassName* getVPtr() const                                                                                                                    \
    {                                                                                                                                                          \
        return reinterpret_cast<const ClassName*>(v_ptr);                                                                                                      \
    }

// pimpl模式中，将d_ptr转换为派生类的具体IMPL类型指针，类似于Qt中的Q_D宏
#define VN_D(ClassName) auto* const d = getDPtr();
// pimpl模式中，将v_ptr转换为派生类具体类型指针，类似于Qt中的Q_Q宏
#define VN_V(ClassName) auto* const v = getVPtr();

// 定义类的侵入式指针类型，并前向声明类（需先包含 <vine/intrusive_ptr.hpp>）
#define VN_DEFINE_PTR(ClassName)                                                                                                                                \
    class ClassName;                                                                                                                                           \
    using ClassName##SharedPtr = intrusive_ptr<ClassName>;                                                                                                    \
    // using ClassName##WeakPtr   = std::weak_ptr<ClassName>;
