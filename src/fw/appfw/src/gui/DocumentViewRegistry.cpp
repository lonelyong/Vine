#include <vine/appfw/gui/DocumentViewRegistry.hpp>

#include <map>

#include <vine/appfw/Document.hpp>
#include <vine/appfw/gui/DocumentView.hpp>

VN_APPFWGUI_NS_BEGIN

struct DocumentViewRegistry::Impl {
    /// Factories by document type id; a map, so types() is ordered by id and does not depend on registration order.
    std::map<String, std::function<DocumentView*(Document&)>> factories;
};

DocumentViewRegistry::DocumentViewRegistry() : d(std::make_unique<Impl>())
{
}

DocumentViewRegistry::~DocumentViewRegistry() = default;

bool DocumentViewRegistry::registerView(const String& document_type_id, std::function<DocumentView*(Document&)> create)
{
    if (document_type_id.empty() || !create) {
        return false;
    }
    if (d->factories.find(document_type_id) != d->factories.end()) {
        // One type is one view (see the header): replacing the factory would leave the views built by the old one
        // answering to a registration nobody can see any more.
        return false;
    }

    d->factories.emplace(document_type_id, std::move(create));
    return true;
}

bool DocumentViewRegistry::hasView(const String& document_type_id) const
{
    return d->factories.find(document_type_id) != d->factories.end();
}

std::vector<String> DocumentViewRegistry::types() const
{
    std::vector<String> result;
    result.reserve(d->factories.size());
    for (const auto& [type_id, factory] : d->factories) {
        static_cast<void>(factory);
        result.push_back(type_id);
    }
    return result;
}

DocumentView* DocumentViewRegistry::create(Document& document) const
{
    const auto factory = d->factories.find(document.typeId());
    if (factory == d->factories.end()) {
        return nullptr;
    }
    return factory->second(document);
}

VN_APPFWGUI_NS_END
