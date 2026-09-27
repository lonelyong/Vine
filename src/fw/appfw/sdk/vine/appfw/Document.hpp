#pragma once

#include "appfw_global.hpp"

#include <vine/Object.hpp>
#include <vine/String.hpp>

VN_APPFW_NS_BEGIN

class DocumentManager;

/**
 * @brief Where a document came from and where save() writes it back: a scheme plus an address.
 *
 * A VALUE on purpose - two strings, nothing more - so a host can compare two documents' sources (is this one already
 * open?), remember the last one, and rebuild a "recent documents" list from a config file. The framework NEVER
 * interprets either string: the SCHEME names who does ("file", "package", "device", ...) and the ADDRESS is whatever
 * that party put there. The type reads its own source back in save(); the opener that built the document built the
 * address.
 *
 * THE EMPTY SOURCE (see valid()) means "this document is not backed by anything": a new document, or one that came
 * from a place there is no writing back to (the clipboard, a generated document). save() cannot succeed for it, and a
 * host offers "save as" instead of "save".
 */
struct VN_APPFW_API DocumentSource {
    /// Who interprets the address; empty means the document has no source at all.
    String scheme;

    /// The rest of the address; its meaning belongs to the scheme and to the document type.
    String address;

    /// @brief Reports whether this source names anything.
    ///
    /// @return true when the document is backed by something a save could write to.
    bool valid() const noexcept { return !scheme.empty(); }

    /// Compares sources by both halves (a host uses this to tell "already open" from "open it again").
    bool operator==(const DocumentSource& other) const = default;
};

/**
 * @brief One open document: its type, its title and its lifecycle - never its presentation.
 *
 * A document is the half of "what the application works on" that does not depend on how it is shown: the
 * DocumentManager owns the open set and asks these four questions, a host decides what to do with the answers, and a
 * document TYPE (usually owned by the plugin that can read and write it) answers them. WHAT the document holds - a
 * mesh, a robot, a table, a picture - is not the framework's business, and neither is whether it is shown in a 3D
 * view, a table or anything else: that belongs to a view type, and how many views a host puts on screen belongs to
 * the host.
 *
 * WHAT THIS CLASS DELIBERATELY DOES NOT HAVE: a window, a widget, a scene, a render area, a path, a "save
 * as" dialog, or a prompt. Asking the user is the application's business (it is the half that has a user interface),
 * which is also why canClose() is a QUERY: the manager refuses the close, and the application decides what to do about
 * the refusal - save first, ask, or leave it open.
 *
 * THREAD AFFINITY: a document is an application-thread object, like the widgets and graphics objects a view builds
 * from it. A document type whose data is expensive to read may load it on the pool, but it must hand the result back
 * to the application thread before the document is created or mutated - the same rule the demo's cube maps follow.
 *
 * OWNERSHIP: the DocumentManager owns every open document (close() is what destroys it). A caller holding a
 * Document* must treat it as borrowed: the pointer is valid until the document is closed, and the close's event is the
 * last moment it can be used.
 */
class VN_APPFW_API Document : public Object {
    VN_OBJECT_META_DECL;
    VN_DISABLE_COPY_MOVE(Document);
    friend class DocumentManager;

  public:
    Document();
    ~Document() override;

  public:
    /**
     * @brief Returns the registered type this document belongs to.
     *
     * This is the id the type was registered under (DocumentManager::registerType()), and create() checks that the
     * instance agrees with its registration: a factory that returns a document reporting a different type is refused
     * rather than entering the open set under a name it does not answer to.
     *
     * @return The type id; never empty for a document the manager accepted.
     */
    virtual String typeId() const = 0;

    /**
     * @brief Returns the title a host shows wherever it presents this document.
     *
     * @return The display title; may change over the document's life (a document that learns its name from a file it
     *         was opened from reports it here).
     */
    virtual String title() const = 0;

    /**
     * @brief Reports whether the document holds changes that are written nowhere yet.
     *
     * PURE ON PURPOSE: every document type has to say what "unsaved" means for it. The framework cannot guess it - a
     * scratch document is never dirty, a loaded file is dirty as soon as it is edited - and a wrong default here
     * either loses a user's work silently or keeps asking about a document that has nothing to save.
     *
     * @return true when the document has changes a save would write.
     */
    virtual bool isDirty() const = 0;

    /**
     * @brief Reports whether the document may be closed now.
     *
     * The default is the conservative one: anything that is not dirty closes, and a dirty document is refused - the
     * manager reports that by returning false from close() and leaving the document open (and current) for the
     * application to deal with. A type that may close while dirty (it auto-saves, or its changes are disposable)
     * overrides this; a type that wants the user's answer does NOT override it (asking is the application's job).
     *
     * @return true to let close() drop the document, false to refuse it.
     */
    virtual bool canClose() const;

  public:
    /**
     * @brief Returns where this document came from, as save() understands it.
     *
     * @return The source recorded when the document was opened (or when a save-as completed); an INVALID source (see
     *         DocumentSource::valid()) for a document that is not backed by anything.
     */
    DocumentSource source() const noexcept;

    /**
     * @brief Writes the document back to its source.
     *
     * WHAT "BACK" MEANS IS THE TYPE'S BUSINESS: a document opened from a file tree writes into that tree, one attached
     * to a device pushes the state to the device, and a document with no source cannot be saved at all. The framework
     * only calls this (a "save" command, or the path a host takes before closing a dirty document) and reports the
     * outcome - it knows nothing about where the bytes go.
     *
     * The default refuses, because a type that never writes anywhere is legitimate (a scratch or generated document)
     * and the framework must not pretend it saved something.
     *
     * @return true when the source now holds the document's current state, false when there is no source, the type
     *         cannot write one, or the write failed (a failure is logged by the type).
     */
    virtual bool save();

  protected:
    /**
     * @brief Records where this document came from, or clears it.
     *
     * Called by the type - it is the half that can turn a payload into an address - when it was opened, and again by a
     * successful save-as. The framework only reads it (Document::source()).
     *
     * @param source The source to record; an invalid one (see DocumentSource::valid()) clears it.
     */
    void setSource(DocumentSource source);

  private:
    /// Where this document came from; invalid when it is backed by nothing.
    DocumentSource source_;
};

VN_APPFW_NS_END
