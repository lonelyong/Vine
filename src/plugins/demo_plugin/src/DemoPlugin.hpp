#pragma once

#include <vine/appfw/Plugin.hpp>

namespace vn::appfw
{
class Application;
}

namespace vn::demo
{

/**
 * @brief Demo plugin: the demo scene as a DOCUMENT (type `"demo"`).
 *
 * The demo used to be something the shell built at every startup; it is a document now, like any other content
 * (appfw-document-model.md §12):
 *
 *  - `load()` registers the document type (with a create factory - the demo needs no payload) and the view that
 *    renders it. Nothing heavy happens here, and nothing at all happens until that document is opened;
 *  - `open_demo` (see commands/OpenDemoCommand) opens it, and `Vine --open demo` does the same at startup - both go
 *    through `DocumentManager::create()`, so "who may create this" stays the type's own business;
 *  - `DemoView` (src/DemoView.hpp) owns the render surface and builds the skeleton, the session and the content when
 *    the document is first shown. The shell no longer creates a render control of its own for it.
 *
 * Exactly one demo document exists at a time: the command switches to the open one instead of creating a second.
 */
class DemoPlugin : public vn::appfw::Plugin {
    VN_OBJECT_META_DECL;

  public:
    DemoPlugin();

  public:
    /**
     * @brief Registers the demo's document type and its view.
     *
     * @param context Load context exposing host capabilities.
     * @return A task that completes once the type and the view are registered.
     */
    vn::async::Task<void> load(vn::appfw::PluginLoadContext* context) override;

  private:
    /**
     * @brief Registers the `"demo"` document type, with the create factory that makes it openable.
     *
     * @param app Host application owning the document manager.
     */
    static void registerDocumentType(vn::appfw::Application& app);

    /**
     * @brief Registers the demo's view (GUI hosts only - a view is a UIElement).
     *
     * @param app Host application owning the view registry.
     */
    static void registerDemoView(vn::appfw::Application& app);
};

}  // namespace vn::demo
