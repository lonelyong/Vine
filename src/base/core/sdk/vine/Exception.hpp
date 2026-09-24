#pragma once

#include "core_global.hpp"

#include <stdexcept>

#include "String.hpp"

VN_CORE_NS_BEGIN

class VN_CORE_API Exception : public std::exception {
  public:
    enum Code
    {
        SYSTEM_ERROR = 0x1,
        INDEX_OUT_OF_RANGE,
        ITEM_ALREADY_EXISTS,
        ARGUMENT_NULL,
        INVALID_ARGUMENTS,
        INVALID_OPERATION,
        NOT_SUPPORT,
        NOT_IMPLEMENTED,
        CANCELLED,
        TIMEOUT,

        USER_EXCEPTION = 0x800001
    };

  public:
    Exception() noexcept {};
    explicit Exception(int code) noexcept;

    Exception(int code, const String& msg) noexcept
      : code_(code)
      , msg_(msg)
    {}

  public:
    [[nodiscard]]
    int code() const noexcept
    {
        return code_;
    }

    [[nodiscard]]
    const String& msg() const noexcept
    {
        return msg_;
    }

    [[nodiscard]]
    const char* what() const noexcept override
    {
        return reinterpret_cast<const char*>(msg_.data());
    }

  private:
    int    code_;
    String msg_;
};

VN_CORE_NS_END

#define VN_CHECK_NULL_THROW(var)                                                                                                                                \
    if (!var)                                                                                                                                                  \
        throw vn::Exception(vn::Exception::ARGUMENT_NULL, u8## #var);
#define VN_CHECK_NULL_RETURN(var)                                                                                                                               \
    if (!var)                                                                                                                                                  \
        return {};
