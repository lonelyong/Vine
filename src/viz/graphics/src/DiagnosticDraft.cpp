#include <vine/graphics/DiagnosticDraft.hpp>

#include <utility>

VN_GRAPHICS_NS_BEGIN

DiagnosticDraft::DiagnosticDraft() = default;

void DiagnosticDraft::refuse(const vn::graphics::DiagnosticCategory category,
                             const char8_t* const             code,
                             vn::String                       message)
{
    refuse(category, code, vn::String(), std::nullopt, std::move(message));
}

void DiagnosticDraft::refuse(const vn::graphics::DiagnosticCategory category,
                             const char8_t* const             code,
                             vn::String                       subject,
                             vn::String                       message)
{
    refuse(category, code, std::move(subject), std::nullopt, std::move(message));
}

void DiagnosticDraft::refuse(const vn::graphics::DiagnosticCategory             category,
                             const char8_t* const                               code,
                             vn::String                                         subject,
                             std::optional<vn::graphics::DiagnosticLimit>       limit,
                             vn::String                                         message)
{
    // THE FIRST REASON IS THE ONE THAT EXPLAINS THE CALL: a later stage that fails as a consequence must
    // not replace the cause that came first (the planners used to spell this rule out with
    // `if (why.empty())` at every call site).
    if (isRefused()) {
        return;
    }
    category_ = category;
    code_     = code;
    subject_  = std::move(subject);
    limit_    = limit;
    message_  = std::move(message);
}

void DiagnosticDraft::clear() noexcept
{
    category_ = vn::graphics::DiagnosticCategory::ContentSkipped;
    code_     = nullptr;
    subject_  = vn::String();
    limit_    = std::nullopt;
    message_  = vn::String();
}

bool DiagnosticDraft::isRefused() const noexcept
{
    // A CODE IS THE REFUSAL: `refuse` always states one, and the sentence beside it may be empty. "Refused"
    // and "has a code" are therefore the same question - which is what keeps a caller from reporting a
    // sentence with no cause.
    return code_ != nullptr;
}

vn::graphics::DiagnosticCategory DiagnosticDraft::category() const noexcept
{
    return category_;
}

const char8_t* DiagnosticDraft::code() const noexcept
{
    return code_;
}

const vn::String& DiagnosticDraft::subject() const noexcept
{
    return subject_;
}

const std::optional<vn::graphics::DiagnosticLimit>& DiagnosticDraft::limit() const noexcept
{
    return limit_;
}

const vn::String& DiagnosticDraft::message() const noexcept
{
    return message_;
}

vn::graphics::RenderDiagnostic DiagnosticDraft::diagnostic(const vn::graphics::DiagnosticSeverity severity) const
{
    vn::graphics::RenderDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.category = category_;
    diagnostic.code     = code_ != nullptr ? vn::String(code_) : vn::String();
    diagnostic.message  = message_;
    diagnostic.subject  = subject_;
    diagnostic.limit    = limit_;
    return diagnostic;
}

VN_GRAPHICS_NS_END
