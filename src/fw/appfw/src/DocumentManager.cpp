#include <vine/appfw/DocumentManager.hpp>

#include <algorithm>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/Document.hpp>
#include <vine/logging/Log.hpp>

VN_APPFW_NS_BEGIN

namespace
{

// UTF-8 view of a text for logging, without allocating (the same one-line helper ConfigManager.cpp and
// PluginManager.cpp carry; vn::String is UTF-8-backed, so this is a view over the same bytes).
std::string_view toUtf8View(const String& s) noexcept
{
    return { reinterpret_cast<const char*>(s.data()), s.size() };
}

/// Fires one event without letting a handler's exception reach the manager (a handler is user code: see the class
/// note, and CommandManager for the same wrapper).
void fireEvent(Event<DocumentManager, DocumentEventArgs>& event, DocumentManager& owner, DocumentEventArgs& args)
{
    try {
        event.trigger(owner, args);
    }
    catch (const std::exception& e) {
        VN_LOGE("document event handler threw: {}", e.what());
    }
    catch (...) {
        VN_LOGE("document event handler threw");
    }
}

/// One registered type: what to show for it, who registered it and how to make one.
struct DocumentType {
    String                     display_name;
    String                     description;
    String                     icon;
    String                     owner;
    std::function<Document*()> create; ///< May be empty: a type that can only be opened.
};

/// How specific a registered payload type is: its depth in its own chain (Object::desc() is 0).
std::size_t payloadDepth(TypeId payload_type) noexcept
{
    // Only ancestors of a payload can match it (see the match in open()), so the deeper the registered type, the
    // closer it sits to the payload - and a payload hierarchy orders itself without the framework knowing it exists.
    std::size_t depth = 0;
    for (const Type* ancestor = payload_type->parent(); ancestor != nullptr; ancestor = ancestor->parent()) {
        ++depth;
    }
    return depth;
}

/// One registered opener: the payload type it takes, how it is ordered, and how it builds a document.
struct Opener {
    String                                          type_id;
    TypeId                                          payload_type; ///< The key: the payload's own type metadata.
    std::size_t                                     payload_depth = 0; ///< How specific that key is (see addOpener()).
    int                                             priority = 0;
    String                                          source_scheme;
    std::function<Document*(raw_ptr<const Object>)> open;
    std::function<int(raw_ptr<const Object>)>       refine; ///< May be empty: accept with score 1.
};

} // namespace

VN_OBJECT_META_IMPL(DocumentEventArgs, EventArgs)

DocumentEventArgs::DocumentEventArgs(Document* document) : document_(document)
{
}

Document* DocumentEventArgs::document() const
{
    return document_;
}

struct DocumentManager::Impl {
    /// Application this manager belongs to, or nullptr for a standalone one (only the registration-owner tag needs it).
    Application* app = nullptr;

    /// Registered types by id; a map, so types() is ordered by id and does not depend on registration order.
    std::map<String, DocumentType> types;

    /// Registered openers, in registration order (a matching pass sorts a copy).
    std::vector<Opener> openers;

    /// The open documents, in creation order. unique_ptr: the manager owns them and close() destroys them.
    std::vector<std::unique_ptr<Document>> documents;

    /// The selected document, or nullptr. Borrowed: it is one of `documents`.
    Document* current = nullptr;

    auto find(Document* document)
    {
        return std::find_if(documents.begin(), documents.end(), [document](const std::unique_ptr<Document>& held) {
            return held.get() == document;
        });
    }

    /// 一个文档进集合的唯一入口：create() 与 openPayload() 都把校验过的实例交给它（保证两者的行为一致）。
    Document* adopt(std::unique_ptr<Document> created, const String& type_id, const String& origin);
};

Document* DocumentManager::Impl::adopt(std::unique_ptr<Document> created, const String& type_id, const String& origin)
{
    if (created == nullptr) {
        VN_LOGW("{} of document type '{}' returned no document", toUtf8View(origin), toUtf8View(type_id));
        return nullptr;
    }
    if (created->typeId() != type_id) {
        // 注册与实例必须一致：管理器里的一切都以 id 为键，报另一个 id 的文档会被列在它并不应答的类型下。
        VN_LOGW("{} registered as '{}' created a '{}' document; refused", toUtf8View(origin), toUtf8View(type_id),
                toUtf8View(created->typeId()));
        return nullptr;
    }

    Document* document = created.get();
    documents.push_back(std::move(created));
    return document;
}

DocumentManager::DocumentManager(Application* owner) : d(std::make_unique<Impl>())
{
    d->app = owner;
}

DocumentManager::~DocumentManager() = default;

bool DocumentManager::registerType(const DocumentTypeRegistration& registration)
{
    if (registration.type_id.empty()) {
        return false;
    }
    if (d->types.find(registration.type_id) != d->types.end()) {
        // One id is one type. Replacing the factory would leave documents the previous one built answering to an id
        // that no longer means what they are.
        VN_LOGW("document type '{}' is already registered; the second registration is ignored",
                toUtf8View(registration.type_id));
        return false;
    }

    // The tag the plugin loader sets around a plugin's registration (Application::registrationOwner()), so a manager
    // panel can say which plugin contributed this type.
    const String owner = d->app != nullptr ? d->app->registrationOwner() : String();

    d->types.emplace(registration.type_id,
                     DocumentType{ registration.display_name, registration.description, registration.icon, owner,
                                   registration.create });
    return true;
}

bool DocumentManager::addOpener(TypeId payload_type, const String& type_id, int priority, String source_scheme,
                                std::function<Document*(raw_ptr<const Object>)> open,
                                std::function<int(raw_ptr<const Object>)> refine)
{
    if (type_id.empty() || !open || payload_type == nullptr) {
        return false;
    }
    if (d->types.find(type_id) == d->types.end()) {
        // A payload nobody can build is worse than no registration: it looks like it works until something opens one.
        VN_LOGW("an opener for the unregistered document type '{}' was refused", toUtf8View(type_id));
        return false;
    }

    d->openers.push_back(Opener{ type_id,
                                 payload_type,
                                 payloadDepth(payload_type),
                                 priority,
                                 std::move(source_scheme),
                                 std::move(open),
                                 std::move(refine) });
    return true;
}

bool DocumentManager::isTypeRegistered(const String& type_id) const
{
    return d->types.find(type_id) != d->types.end();
}

std::vector<DocumentTypeInfo> DocumentManager::types() const
{
    std::vector<DocumentTypeInfo> result;
    result.reserve(d->types.size());
    for (const auto& [type_id, type] : d->types) {
        DocumentTypeInfo info;
        info.type_id      = type_id;
        info.display_name = type.display_name;
        info.description  = type.description;
        info.icon         = type.icon;
        info.owner        = type.owner;
        info.can_create   = (type.create != nullptr);

        // What the type can be built FROM: filled from the openers, so a panel answers "can anything open this?"
        // without the framework knowing a single payload type. The name is the payload's own (its metadata), not a
        // second name supplied at registration, so the two cannot disagree.
        for (const Opener& opener : d->openers) {
            if (opener.type_id != type_id) {
                continue;
            }
            info.payload_types.push_back(opener.payload_type->fullName());
            if (!opener.source_scheme.empty()
                && std::find(info.source_schemes.begin(), info.source_schemes.end(), opener.source_scheme)
                       == info.source_schemes.end()) {
                info.source_schemes.push_back(opener.source_scheme);
            }
        }

        result.push_back(std::move(info));
    }
    return result;
}

Document* DocumentManager::open(raw_ptr<const Object> payload)
{
    if (payload == nullptr) {
        return nullptr; // "Nothing to open from" is not an error: no opener is asked and nothing is reported.
    }

    // The key is the payload's own metadata. Compared with Type::operator==, which goes down to the C++ type_info: a
    // healthy process holds exactly one Type per C++ type (a second construction throws), and this keeps matching even
    // in the unlikely case of two modules having ended up with two Type objects for the same payload type.
    const Type* payload_type = payload->getType();

    /// A candidate is an INDEX into d->openers, never a pointer into it: user code runs below (refine, open), and
    /// registerOpener() appends to that vector, which reallocates it and would leave a pointer dangling mid-call.
    struct Candidate {
        std::size_t order;
        int         score;
    };

    // Snapshot: an opener registered while this call runs does not join it (the rule EventBus::publish() follows for a
    // subscription added during a dispatch), so a single open cannot see a half-updated registry.
    const std::size_t      count = d->openers.size();
    std::vector<Candidate> candidates;
    for (std::size_t index = 0; index < count; ++index) {
        // Registering a type accepts it AND anything derived from it (the same "is a kind of" EventBus substitutes
        // on), so a payload hierarchy needs no support from the framework: the opener gets the type it registered for.
        if (!payload_type->isKindOf(d->openers[index].payload_type)) {
            continue;
        }

        // Called on a copy OF THE OPENER (never of the payload - the framework does not copy payloads): refine is user
        // code and may register another opener, which moves the std::function that is running right now out from under
        // us.
        const std::function<int(raw_ptr<const Object>)> refine = d->openers[index].refine;

        // No refine means "I take every payload of my type"; one that says 0 or below refuses it.
        const int score = refine ? refine(payload) : 1;
        if (score <= 0) {
            continue;
        }
        candidates.push_back(Candidate{ index, score });
    }

    if (candidates.empty()) {
        // Naming the payload type is what turns "it did not open" into "nothing here takes this thing".
        VN_LOGW("no opener accepts a payload of type '{}'", toUtf8View(payload_type->fullName()));
        return nullptr;
    }

    std::stable_sort(candidates.begin(), candidates.end(), [this](const Candidate& left, const Candidate& right) {
        const Opener& left_opener  = d->openers[left.order];
        const Opener& right_opener = d->openers[right.order];

        // Specificity first: "this kind of payload is mine" is a structural statement, and a content-based score must
        // not steal a payload from the opener that registered for the payload's own type.
        if (left_opener.payload_depth != right_opener.payload_depth) {
            return left_opener.payload_depth > right_opener.payload_depth;
        }
        if (left.score != right.score) {
            return left.score > right.score; // A finer judgement wins over a coarse one.
        }
        if (left_opener.priority != right_opener.priority) {
            return left_opener.priority > right_opener.priority;
        }
        return left.order < right.order; // Same payload, same priority: whoever registered first.
    });

    for (const Candidate& candidate : candidates) {
        // Opener copied again (not the payload), for the same reason as refine above: open is user code too. Re-read
        // through the index, never through a reference taken before a call that could have grown the vector.
        const std::function<Document*(raw_ptr<const Object>)> open    = d->openers[candidate.order].open;
        const String                                          type_id = d->openers[candidate.order].type_id;

        Document* document = d->adopt(std::unique_ptr<Document>(open(payload)), type_id, u8"an opener");
        if (document == nullptr) {
            continue; // The opener failed (it logged why); a later candidate may still build one.
        }

        DocumentEventArgs args(document);
        fireEvent(opened, *this, args);

        // A handler may have closed it again ("it turned out not to be openable"): then the document is gone from the
        // open set and destroyed, and handing it back would hand back a dangling pointer. The contract is "a usable
        // document or nullptr", never "a pointer that was valid a moment ago".
        if (d->find(document) == d->documents.end()) {
            return nullptr;
        }
        return document;
    }
    return nullptr;
}

Document* DocumentManager::create(const String& type_id)
{
    const auto type = d->types.find(type_id);
    if (type == d->types.end()) {
        VN_LOGW("no document type is registered as '{}'", toUtf8View(type_id));
        return nullptr;
    }
    if (!type->second.create) {
        // Legitimate: a type that can only be opened (it has no "empty" state to start from).
        VN_LOGW("document type '{}' has no create factory; it can only be opened", toUtf8View(type_id));
        return nullptr;
    }

    Document* document = d->adopt(std::unique_ptr<Document>(type->second.create()), type_id, u8"the factory");
    if (document == nullptr) {
        return nullptr;
    }

    DocumentEventArgs args(document);
    fireEvent(opened, *this, args);

    // Same reason as in openPayload(): a handler is free to close the document it was just told about, and then this
    // pointer is not a document any more.
    if (d->find(document) == d->documents.end()) {
        return nullptr;
    }
    return document;
}

std::vector<Document*> DocumentManager::documents() const
{
    std::vector<Document*> result;
    result.reserve(d->documents.size());
    for (const auto& held : d->documents) {
        result.push_back(held.get());
    }
    return result;
}

Document* DocumentManager::current() const
{
    return d->current;
}

bool DocumentManager::setCurrent(Document* document)
{
    if (document != nullptr && d->find(document) == d->documents.end()) {
        return false;
    }
    if (d->current == document) {
        return true;
    }

    d->current = document;

    DocumentEventArgs args(document);
    fireEvent(currentChanged, *this, args);
    return true;
}

bool DocumentManager::close(Document* document)
{
    const auto held = d->find(document);
    if (held == d->documents.end()) {
        return false;
    }
    if (!document->canClose()) {
        // Refused: the open set and the selection are untouched, so the caller can save and try again.
        return false;
    }

    // Out of the set FIRST (a handler that walks documents() sees the set as it is now), but alive until the end of
    // this function: the events below are the last moment a handler may read the document.
    std::unique_ptr<Document> closing = std::move(*held);
    d->documents.erase(held);

    const bool was_current = (d->current == document);
    if (was_current) {
        // BEFORE `closed` on purpose: a host that picks a replacement in its `closed` handler would otherwise have its
        // choice overwritten by this very notification (see the header).
        d->current = nullptr;
        DocumentEventArgs cleared(nullptr);
        fireEvent(currentChanged, *this, cleared);
    }

    DocumentEventArgs args(document);
    fireEvent(closed, *this, args);
    return true;
}

VN_APPFW_NS_END
