#include <vine/backend_vk/internal/ContentRefusal.hpp>

#include <utility>

VN_BACKEND_VK_NS_BEGIN

ContentRefusal::ContentRefusal() = default;

void ContentRefusal::refuse(const vn::graphics::DiagnosticCategory category, const char8_t* const code, vn::String text)
{
    // THE FIRST REASON IS THE ONE THAT EXPLAINS THE DRAW: a second stage that fails as a consequence must not replace
    // the cause that came first (the planner used to spell this rule out with `if (why.empty())` at every call site).
    if (isRefused()) {
        return;
    }
    category_ = category;
    code_     = code;
    message_  = std::move(text);
}

void ContentRefusal::clear() noexcept
{
    category_ = vn::graphics::DiagnosticCategory::ContentSkipped;
    code_     = nullptr;
    message_  = vn::String();
}

bool ContentRefusal::isRefused() const noexcept
{
    // A CODE IS THE REFUSAL: `refuse` always states one, and the sentence beside it may be empty (a cause whose code
    // is the whole message). "Refused" and "has a code" are therefore the same question - which is what keeps a
    // caller from reporting a sentence with no cause.
    return code_ != nullptr;
}

vn::graphics::DiagnosticCategory ContentRefusal::category() const noexcept
{
    return category_;
}

const char8_t* ContentRefusal::code() const noexcept
{
    return code_;
}

const vn::String& ContentRefusal::message() const noexcept
{
    return message_;
}

VN_BACKEND_VK_NS_END
