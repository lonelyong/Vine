#include <vine/appfw/gui/DocumentView.hpp>

#include <vine/appfw/Document.hpp>

VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(DocumentView, UIElement)

DocumentView::DocumentView(Document& document)
  : UIElement(nullptr)
  , document_(&document)
{
}

DocumentView::DocumentView(Document& document, UIObject* impl)
  : UIElement(impl)
  , document_(&document)
{
}

DocumentView::~DocumentView() = default;

Document& DocumentView::document() const noexcept
{
    return *document_;
}

VN_APPFWGUI_NS_END
