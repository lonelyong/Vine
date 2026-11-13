#include <vine/graphics/PassRegistry.hpp>

#include <vine/graphics/Plan.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/Scene.hpp>

VN_GRAPHICS_NS_BEGIN

PassRegistry::PassRegistry(Lease<RenderEngine> engine) noexcept
  : engine_(engine)
{}

bool PassRegistry::isValid() const noexcept
{
    return engine_.get() != nullptr;
}

std::vector<PlanProblem> PassRegistry::apply(const Plan& plan, const void* owner)
{
    if (raw_ptr<RenderEngine> engine = engine_.get()) {
        return engine->applyPlan(plan, owner);
    }
    return { PlanProblem{ PlanProblemCode::SessionBusy, String(), String(u8"this handle's engine is gone") } };
}

raw_ptr<RenderPass> PassRegistry::find(const String& name) const
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->pass(name) : nullptr;
}

raw_ptr<RenderPass> PassRegistry::findFor(const String& name, const void* owner) const
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->passFor(name, owner) : nullptr;
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

bool PassRegistry::bindContent(raw_ptr<RenderPass> pass, intrusive_ptr<Scene> content)
{
    if (raw_ptr<RenderEngine> engine = engine_.get()) {
        return engine->bindPassContent(pass, std::move(content));
    }
    return false;
}

raw_ptr<Scene> PassRegistry::contentOf(raw_ptr<RenderPass> pass) const
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->contentOf(pass) : nullptr;
}

void PassRegistry::clearOwnedBy(const void* owner) noexcept
{
    if (raw_ptr<RenderEngine> engine = engine_.get()) {
        engine->removePassesOwnedBy(owner);
    }
}

PassRegistry RenderEngine::passes() noexcept
{
    return PassRegistry(lease());
}

VN_GRAPHICS_NS_END
