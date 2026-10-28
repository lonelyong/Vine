#include <vine/graphics/OutputRegistry.hpp>

#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderTarget.hpp>

VN_GRAPHICS_NS_BEGIN

OutputRegistry::OutputRegistry(Lease<RenderEngine> engine) noexcept
  : engine_(engine)
{}

bool OutputRegistry::isValid() const noexcept
{
    return engine_.get() != nullptr;
}

void OutputRegistry::publish(const String& name, intrusive_ptr<RenderTarget> target)
{
    if (raw_ptr<RenderEngine> engine = engine_.get()) {
        engine->publish(name, std::move(target));
    }
}

raw_ptr<RenderTarget> OutputRegistry::resolve(const String& name) const
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->resolve(name) : nullptr;
}

void OutputRegistry::unpublish(const String& name)
{
    if (raw_ptr<RenderEngine> engine = engine_.get()) {
        engine->unpublish(name);
    }
}

raw_ptr<RenderTarget> OutputRegistry::transientTarget(const String& identity) const
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->transientTarget(identity) : nullptr;
}

std::size_t OutputRegistry::transientCount() const noexcept
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->transientTargetCount() : 0;
}

OutputRegistry RenderEngine::outputs() noexcept
{
    return OutputRegistry(lease());
}

VN_GRAPHICS_NS_END
