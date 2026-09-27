#pragma once

#include "appfw_global.hpp"

#include <functional>
#include <memory>
#include <vector>

#include <vine/Events.hpp>
#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/Type.hpp>
#include <vine/raw_ptr.hpp>

VN_APPFW_NS_BEGIN

class Application;
class Document;

/**
 * @brief A type that may be registered as a payload: an Object that carries type metadata of its own.
 *
 * A payload's own Type is the key an opener is registered under (registerOpener()) and what openPayload() matches a
 * concrete payload against, which is why the metadata has to be the payload's own and not an inherited one: without
 * VN_OBJECT_META_DECL, desc() would resolve to Object::desc() and key every such payload on the same type. Declaring
 * it is that half of the rule - the second half is VN_OBJECT_META_IMPL in exactly one translation unit, which the type
 * metadata enforces itself rather than the framework: Type refuses a second Type for the same C++ type. So a payload is
 * deliberately not header-only.
 */
template <typename T>
concept DocumentPayload = ObjectBased<T> && TypeDescribed<T> && (&T::desc != &Object::desc);

/**
 * @brief Document event arguments: carry the document the event is about.
 *
 * `opened` and `closed` carry the document that entered or left the open set; `currentChanged` carries the NEW current
 * document, which is null when the manager has none. A handler may read the document (its title, its dirty state) but
 * must not assume it stays alive after the event returns - for `closed` this event is the last moment the document
 * exists at all.
 */
class VN_APPFW_API DocumentEventArgs : public EventArgs {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Wraps the document an event is about.
     *
     * @param document Document the event is about; null means "no current document" for currentChanged.
     */
    explicit DocumentEventArgs(Document* document);

    /**
     * @brief Returns the document the event is about.
     *
     * @return The document, or nullptr for a currentChanged that means "none".
     */
    Document* document() const;

  private:
    Document* document_;
};

/**
 * @brief What a plugin registers for one document type: how to show it and how to make one.
 *
 * A struct rather than a parameter list because every field but the id is optional and named at the call site (see
 * CommandInfo for the same reasoning on the listing side).
 */
struct DocumentTypeRegistration {
    /// Id create() and registerOpener() take, and what Document::typeId() must report; required, never empty.
    String type_id;

    /// Name to show a user (may be empty; a host falls back to the id).
    String display_name;

    /// Longer text for a manager panel or a tooltip (may be empty).
    String description;

    /// Inline SVG, the same form VN_DECLARE_PLUGIN takes (may be empty).
    String icon;

    /// Creates a document with NO source (a "new document"); may be null for a type that can only be opened.
    std::function<Document*()> create;
};

/**
 * @brief One registered document type, as a manager panel needs it.
 *
 * The registration fields, plus what the registry knows and the type cannot say for itself: who registered it, whether
 * it can be created from nothing, and what it can be built FROM (the payload list, filled by registerOpener()).
 */
struct DocumentTypeInfo {
    /// Id create() and openers take.
    String type_id;
    /// Name to show a user; empty when the type registered none.
    String display_name;
    /// Longer text; empty when the type registered none.
    String description;
    /// Inline SVG; empty when the type registered none.
    String icon;
    /// Name of the plugin that registered the type; empty for a host registration.
    String owner;
    /// Whether registerType() was given a create factory (false: the type can only be opened).
    bool can_create = false;
    /// Fully qualified names of the payload types this type's openers accept, from each payload's own metadata (the
    /// payload name is not registered separately: the type is the key, so its name cannot drift from it).
    std::vector<String> payload_types;
    /// Source schemes this type's openers produce (see DocumentSource::scheme); empty for a type with no openers.
    std::vector<String> source_schemes;
};

/**
 * @brief What an opener registration carries: the document type it builds and how it builds one from a payload.
 *
 * A struct rather than a parameter list for the same reason DocumentTypeRegistration is one: every field but the id and
 * the callback is optional, and named fields at the call site cannot be swapped for one another - which two
 * std::function parameters of different signatures certainly can. It is templated on the payload type because that is
 * what the framework keys the registration under and what makes the callbacks typed: an opener receives its own payload
 * type, never an Object it has to cast itself.
 *
 * A designated initializer list has to follow the declaration order below (C++20): type_id, priority, open,
 * source_scheme, refine.
 */
template <typename Payload>
struct DocumentOpenerRegistration {
    /// Id of the registered type this opener builds (see registerType()); required, never empty, and registered first.
    String type_id;

    /// Tie-breaker among openers of the same payload, higher first; may be negative (see registerOpener()).
    int priority = 0;

    /// Builds the document from the payload; required (a registration without one is refused).
    std::function<Document*(const Payload&)> open;

    /// Scheme this opener produces (DocumentSource::scheme), for listing; may be empty.
    String source_scheme;

    /// Optional finer judgement on the payload: a score above 0 accepts it (higher is tried earlier), 0 and below
    /// refuses it. A registration with no refine accepts every payload of its type with score 1.
    std::function<int(const Payload&)> refine;
};

/**
 * @brief Owns the open documents: which types exist, which documents are open, which one is current.
 *
 * The framework's whole document surface, and deliberately no more: it answers "what is open" and "which one is
 * current" and lets the application decide everything else. It knows NOTHING about single- versus multi-document
 * applications (a host that allows one at a time closes the previous one itself), nothing about layout or tabs, and
 * nothing about views: a document type says what it is, a view type says how it is shown, a host decides where.
 *
 * Application holds the single instance (Application::documentManager()).
 *
 * THREAD AFFINITY: application thread only, like the documents it owns. It carries no lock on purpose: the mutations
 * it makes (create / close / setCurrent) are one host's actions on one window, and the events it fires hand user code
 * the manager itself, so a handler is free to call back in - the same "no lock held while running user code" rule the
 * rest of appfw follows.
 */
class VN_APPFW_API DocumentManager {
  public:
    /**
     * @brief Constructs a manager.
     *
     * @param owner Application this manager belongs to, or nullptr for a standalone one (tests, tools). It is used for
     *              one thing: reading the current registration owner tag so a type records which plugin registered it.
     */
    explicit DocumentManager(Application* owner = nullptr);
    virtual ~DocumentManager();

  public:
    /// Fired after a document entered the open set (it is in documents() when a handler runs).
    Event<DocumentManager, DocumentEventArgs> opened;
    /// Fired after a document left the open set (it is gone from documents(), but still alive during this event).
    Event<DocumentManager, DocumentEventArgs> closed;
    /// Fired after current() changed; carries the NEW current document, null for "none".
    Event<DocumentManager, DocumentEventArgs> currentChanged;

  public:
    /**
     * @brief Makes a document type available to this manager.
     *
     * Called by the plugin that owns the type, from its load(). One id is one type: a second registration under a
     * taken id is refused (rather than silently replacing the factory, which would leave the documents already built
     * by the first one unexplained).
     *
     * @param registration The type's id, how to show it and how to create one (see DocumentTypeRegistration).
     * @return true when the type was registered, false when the id is empty or already taken.
     */
    bool registerType(const DocumentTypeRegistration& registration);

    /**
     * @brief Reports whether a type id is registered.
     *
     * @param type_id Id to look up.
     * @return true when registerType() accepted this id.
     */
    bool isTypeRegistered(const String& type_id) const;

    /**
     * @brief Lists the registered types, ordered by id.
     *
     * @return One entry per registered type (for a "new document" menu and for a manager panel); empty when none is
     *         registered.
     */
    std::vector<DocumentTypeInfo> types() const;

  public:
    /**
     * @brief Registers an opener: "this type can be built from THIS payload" (see DocumentOpenerRegistration).
     *
     * THE PAYLOAD IS A PRIVATE PROTOCOL between whoever opens and the type - a value object such as "a virtual file
     * tree plus a path", "an image on the clipboard", "a device handle", or a struct carrying several inputs. It
     * derives from Object (see DocumentPayload) so that its type is one the framework already has a name and identity
     * for, and that Type is the key: it is what this opener is registered under, what open() matches a concrete payload
     * against, and where the name a manager panel shows comes from. This is the same type-erased shape
     * EventBus::subscribe() uses, so appfw has one runtime type system instead of two. The framework never looks
     * inside a payload and never names a payload type of its own.
     *
     * THE PAYLOAD IS BORROWED. The @p open callback receives it by reference and must not keep a pointer to the payload
     * object itself: that object only has to live for the call, while anything it POINTS AT (a Vfs, a device, an image
     * buffer) has to outlive the document the opener builds. What a document does keep is what it copied out of the
     * payload (see open(), and appfw-document-model.md §5.5 and §5.8).
     *
     * THE FRAMEWORK NEVER COPIES A PAYLOAD EITHER: it goes from the caller's object to @p open as one reference, and
     * nothing in between holds it.
     *
     * REGISTERING FOR A TYPE MEANS ACCEPTING THAT TYPE AND ANYTHING DERIVED FROM IT: the payload's own type has to be
     * a kind of the registered one, and @p open always receives the type it registered for - so a payload hierarchy
     * works without the framework knowing anything about it, the same way EventBus delivers a derived event to a
     * subscriber of its base.
     *
     * SEVERAL OPENERS MAY MATCH: the MOST SPECIFIC registration is tried first (the registered type closest to the
     * payload's own type: "this kind of payload is mine", said structurally), then by refinement score (high first, see
     * DocumentOpenerRegistration::refine), then by priority (high first), then in registration order, and the first one
     * that returns a document wins. So a registration for a base type is a fallback that runs when the specific ones
     * refuse or return nothing, and a score never outranks specificity. That is what tells ".urdf" and ".stl" in the
     * same tree apart without the framework knowing about extensions.
     *
     * @tparam Payload      The payload type this opener accepts; the type is the key, and it needs metadata of its own
     *                      (VN_OBJECT_META_DECL here, VN_OBJECT_META_IMPL in one .cpp).
     * @param registration  What to register; every field is documented on DocumentOpenerRegistration.
     * @return true when the opener was registered, false when the type id is empty or not registered, or the
     *         registration carries no open callback.
     */
    template <DocumentPayload Payload>
    bool registerOpener(const DocumentOpenerRegistration<Payload>& registration)
    {
        // Checked here and not in addOpener(): the callback it receives is wrapped below, so it is never the null one.
        if (!registration.open) {
            return false;
        }

        return addOpener(
            Payload::desc(), registration.type_id, registration.priority, registration.source_scheme,
            [open = registration.open](raw_ptr<const Object> payload) -> Document* {
                // The key is Payload::desc() and open() matched the runtime type against it first, so this cast cannot
                // fail - it is the checked way of giving the opener back the type it registered for.
                const Payload* typed = obj_cast<Payload>(payload);
                return typed != nullptr ? open(*typed) : nullptr;
            },
            registration.refine
                ? std::function<int(raw_ptr<const Object>)>(
                      [refine = registration.refine](raw_ptr<const Object> payload) {
                          const Payload* typed = obj_cast<Payload>(payload);
                          return typed != nullptr ? refine(*typed) : 0;
                      })
                : std::function<int(raw_ptr<const Object>)>());
    }

    /**
     * @brief Builds a document from a borrowed payload object, by the openers registered for its runtime type.
     *
     * The key is the payload's own type (payload->getType()), so there is nothing for a caller to get wrong and no
     * typed overload to keep in sync: a template parameter would only restate the type of the reference it was handed.
     * The payload is a pointer the way close() and setCurrent() take pointers - it is borrowed, it is what the caller
     * owns, and nullptr is the legal "nothing to open from".
     *
     * THE PAYLOAD IS BORROWED AND READ DURING THIS CALL ONLY. Two lifetimes are in play, and only the first one is the
     * manager's business:
     *   - the PAYLOAD OBJECT (the descriptor itself, e.g. "a virtual tree plus a path") lives for this call - the
     *     manager never keeps it and a document must not keep a POINTER to it. What it MAY keep is what it needs out of
     *     it: a type that wants to re-read later copies the fields it needs (the Vfs handle, the path) into itself or
     *     into its source - that is the opener's job, and it is what makes a refresh possible without the framework
     *     owning any payload;
     *   - whatever the payload POINTS AT (a Vfs, a device, an image buffer) has to outlive the DOCUMENT, because the
     *     document keeps using it long after this call returned (see appfw-document-model.md §5.5).
     *
     * THE FRAMEWORK NEVER COPIES A PAYLOAD, and it does not help anyone keep one alive either: the copying half is the
     * document type's (copy the fields you need into yourself, see the first bullet) and the keeping half is the
     * caller's (keep the description itself if you want to open from it again later). Nothing in this path hides a copy
     * - the caller's object reaches the opener as one reference.
     *
     * The openers considered are the ones registered when the call started: an opener registered while this call is
     * running (from a refine() or an open() of an earlier candidate) does not join it, the same rule EventBus::publish()
     * follows for a subscription added during a dispatch. A payload matches every opener registered for a type it is a
     * kind of, and they are tried most specific first (see registerOpener()).
     *
     * @param payload Borrowed payload to open from - an lvalue the caller owns (there is deliberately no value form: a
     *                payload is a descriptor that may hold references) - or nullptr for "nothing to open from", which
     *                asks no opener and fires nothing.
     * @return The new document (owned by this manager), or nullptr when no opener accepts the payload, when every
     *         candidate refused, when the document a candidate built does not report the type it was registered as, or
     *         when a handler of `opened` closed that document again before this call returned.
     */
    Document* open(raw_ptr<const Object> payload);

  public:
    /**
     * @brief Creates a document of the given type and adds it to the open set.
     *
     * DOES NOT TOUCH current(): creating and selecting are two decisions, and a host that wants the new document
     * selected says so with setCurrent(). The `opened` event fires after the document is in documents(), so a handler
     * that wants to show it can.
     *
     * @param type_id Id of a registered type that has a create factory (a type that can only be opened reports
     *                can_create == false and cannot be built this way).
     * @return The new document (owned by this manager), or nullptr when the id is not registered, the type has no
     *         create factory, the factory failed, or the instance it returned does not report the type it was
     *         registered as.
     */
    Document* create(const String& type_id);

  public:
    /**
     * @brief Lists the open documents, in the order they were created.
     *
     * @return Borrowed pointers, one per open document; the caller must not keep them past the document's close.
     */
    std::vector<Document*> documents() const;

    /**
     * @brief Returns the current document.
     *
     * "Current" is the manager's answer to "which document is the user working on", for commands and hosts that need
     * one - it says nothing about what is on screen: a host may show the current document, or several, or none.
     *
     * @return The current document, or nullptr when none was selected.
     */
    Document* current() const;

  public:
    /**
     * @brief Selects the current document.
     *
     * @param document Document to select, which must be one this manager owns, or nullptr to clear the selection.
     * @return true when current() now is @p document, false when it is not a document of this manager (the selection
     *         is left untouched). Selecting what is already current is a no-op and fires nothing.
     */
    bool setCurrent(Document* document);

    /**
     * @brief Closes a document: asks it first, removes it, and destroys it.
     *
     * The document is asked through canClose() and a refusal changes NOTHING - it stays open, and current() stays
     * what it was, so a host can let the user save and try again. When it accepts, it leaves the open set before the
     * events fire (a handler that walks documents() sees the set as it is now), is still alive during them (a handler
     * may read it - this is where a host saves what it needs from the document), and is destroyed when close()
     * returns unless the caller kept it alive some other way.
     *
     * A closed document that was current leaves NO current document, and `currentChanged` fires BEFORE `closed`: a
     * host that wants "the next one" as its new current does that in its `closed` handler, and firing it first is what
     * keeps that choice from being overwritten a moment later.
     *
     * @param document Document to close, which must be one this manager owns.
     * @return true when it was closed, false when it is not a document of this manager or it refused (canClose()).
     */
    bool close(Document* document);

  private:
    /// Non-template tail of registerOpener<Payload>(): the template restores the payload type inside both callbacks.
    bool addOpener(TypeId payload_type, const String& type_id, int priority, String source_scheme,
                   std::function<Document*(raw_ptr<const Object>)> open,
                   std::function<int(raw_ptr<const Object>)> refine);

    struct Impl;
    std::unique_ptr<Impl> d;
};

VN_APPFW_NS_END
