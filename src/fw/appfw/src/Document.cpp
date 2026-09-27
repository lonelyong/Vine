#include <vine/appfw/Document.hpp>

#include <utility>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(Document, Object)

Document::Document() = default;
Document::~Document() = default;

bool Document::canClose() const
{
    return !isDirty();
}

DocumentSource Document::source() const noexcept
{
    return source_;
}

bool Document::save()
{
    // No source and no writer: a type that can be saved overrides this (see the header).
    return false;
}

void Document::setSource(DocumentSource source)
{
    source_ = std::move(source);
}

VN_APPFW_NS_END
