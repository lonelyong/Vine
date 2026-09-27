#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <vine/appfw/appfw_global.hpp>

#include <vine/String.hpp>

VN_APPFW_NS_BEGIN
class Document;
VN_APPFW_NS_END

VN_APPFWGUI_NS_BEGIN

class DocumentView;

/**
 * @brief Which view presents which document type - the table the host looks a document up in.
 *
 * GUI-ONLY BY DESIGN: a view is a UIElement, so this table lives where the GUI application does
 * (GuiApplication::viewRegistry()) and the headless Application has none. A run without a window needs no views, and
 * that is the honest shape rather than an empty table nobody could fill.
 *
 * ONE FACTORY PER DOCUMENT TYPE, and the type's owner need not be the one registering: a plugin that owns a view for
 * somebody else's document type registers it here, which is why this is a table and not a virtual on `Document`.
 *
 * A factory is called ONCE PER DOCUMENT INSTANCE by the host; it must return a new view every time (handing the same
 * instance to two documents would give them one camera, one selection and one owner). A type with no registration simply
 * cannot be shown - the host says so where the document would have been instead of pretending.
 *
 * THREAD AFFINITY: application (GUI) thread only, like the documents and the widgets it talks about.
 */
class VN_APPFW_API DocumentViewRegistry {
  public:
    DocumentViewRegistry();
    ~DocumentViewRegistry();

    DocumentViewRegistry(const DocumentViewRegistry&)            = delete;
    DocumentViewRegistry& operator=(const DocumentViewRegistry&) = delete;

  public:
    /**
     * @brief Registers how a document type is presented.
     *
     * One type is one view: a second registration under a taken id is refused rather than silently replacing the
     * first (the views already created from it would keep coming from a factory nobody can see any more).
     *
     * @param document_type_id Id a document reports from Document::typeId(); required, never empty.
     * @param create           Factory building a view for one document; required, and called once per document.
     * @return true when the view was registered, false when the id is empty, the factory is null, or the id is taken.
     */
    bool registerView(const String& document_type_id, std::function<DocumentView*(Document&)> create);

    /**
     * @brief Reports whether a document type can be shown.
     *
     * @param document_type_id Id to look up.
     * @return true when a view is registered for it.
     */
    bool hasView(const String& document_type_id) const;

    /**
     * @brief Lists the document types that have a view, ordered by id.
     *
     * @return The registered type ids; empty when none is registered.
     */
    std::vector<String> types() const;

    /**
     * @brief Builds a view for a document, or reports that its type cannot be shown.
     *
     * @param document Document to present.
     * @return A new view owned by the caller, or nullptr when no view is registered for the document's type.
     */
    DocumentView* create(Document& document) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

VN_APPFWGUI_NS_END
