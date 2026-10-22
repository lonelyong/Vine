#include <vine/graphics/PassRegistry.hpp>

#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/Scene.hpp>

VN_GRAPHICS_NS_BEGIN

PassRegistry::PassRegistry(Lease<RenderEngine> engine) noexcept : engine_(engine)
{
}

bool PassRegistry::isValid() const noexcept
{
    return engine_.get() != nullptr;
}

intrusive_ptr<RenderPass> PassRegistry::add(const PassDesc& desc)
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->addPass(desc) : nullptr;
}

intrusive_ptr<RenderPass> PassRegistry::add(intrusive_ptr<RenderPass> pass, int order)
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->addPass(std::move(pass), order) : nullptr;
}

intrusive_ptr<RenderPass> PassRegistry::add(intrusive_ptr<RenderPass> pass, intrusive_ptr<Scene> content, int order)
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->addPass(std::move(pass), std::move(content), order) : nullptr;
}

void PassRegistry::remove(raw_ptr<RenderPass> pass)
{
    if (raw_ptr<RenderEngine> engine = engine_.get()) {
        engine->removePass(pass);
    }
}

void PassRegistry::clear()
{
    if (raw_ptr<RenderEngine> engine = engine_.get()) {
        engine->clearPasses();
    }
}

std::size_t PassRegistry::count() const
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->passCount() : 0;
}

std::optional<int> PassRegistry::orderOf(raw_ptr<RenderPass> pass) const
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->passOrder(pass) : std::nullopt;
}

void PassRegistry::bindContent(raw_ptr<RenderPass> pass, intrusive_ptr<Scene> content)
{
    if (raw_ptr<RenderEngine> engine = engine_.get()) {
        engine->bindPassContent(pass, std::move(content));
    }
}

raw_ptr<Scene> PassRegistry::contentOf(raw_ptr<RenderPass> pass) const
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->contentOf(pass) : nullptr;
}

intrusive_ptr<RenderPass> PassRegistry::addOwnedBy(const PassDesc& desc, raw_ptr<Object> owner, const String& role)
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->addPassOwnedBy(desc, owner, role) : nullptr;
}

void PassRegistry::removeOwnedBy(const Object* owner) noexcept
{
    if (raw_ptr<RenderEngine> engine = engine_.get()) {
        engine->removePassesOwnedBy(owner);
    }
}

std::vector<intrusive_ptr<RenderPass>> PassRegistry::passesOwnedBy(const Object* owner) const
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->passesOwnedBy(owner) : std::vector<intrusive_ptr<RenderPass>>{};
}

raw_ptr<RenderPass> PassRegistry::passOwnedBy(const Object* owner, const String& role) const
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->passOwnedBy(owner, role) : nullptr;
}

PassRegistry RenderEngine::passes() noexcept
{
    return PassRegistry(lease());
}

VN_GRAPHICS_NS_END
