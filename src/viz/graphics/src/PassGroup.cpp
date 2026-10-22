#include <vine/graphics/PassGroup.hpp>

#include <vine/graphics/PassDesc.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>

VN_GRAPHICS_NS_BEGIN

PassGroup::PassGroup(Lease<RenderEngine> engine, std::uint64_t id) noexcept
  : engine_(engine)
  , id_(id)
{
}

PassGroup::PassGroup(PassGroup&& other) noexcept
  : engine_(other.engine_)
  , id_(other.id_)
{
    // The registration travels with the handle: the source keeps its lease but loses the id, so it owns nothing and
    // its destructor has nothing to remove (two owners would remove the same passes twice).
    other.id_ = 0;
}

PassGroup& PassGroup::operator=(PassGroup&& other) noexcept
{
    if (this != &other) {
        release();  // what THIS handle held is not @p other's to keep
        engine_   = other.engine_;
        id_       = other.id_;
        other.id_ = 0;
    }
    return *this;
}

PassGroup::~PassGroup()
{
    release();
}

void PassGroup::release() noexcept
{
    // The engine may be gone (a group can outlive the thing it registered on): the slot table died with it, so
    // there is nothing left to remove. The release path the removal takes (backend state, an owned target) is the
    // engine's own (see RenderEngine::removePass), which is why a group does not duplicate it.
    raw_ptr<RenderEngine> engine = engine_.get();
    if (engine != nullptr && id_ != 0) {
        engine->removePassGroup(id_);
    }
    id_ = 0;
}

bool PassGroup::isValid() const noexcept
{
    return id_ != 0 && engine_.get() != nullptr;
}

intrusive_ptr<RenderPass> PassGroup::add(const PassDesc& desc, const String& role)
{
    raw_ptr<RenderEngine> engine = engine_.get();
    if (engine == nullptr) {
        return nullptr;
    }
    return engine->addPassToGroup(desc, id_, role);
}

std::vector<intrusive_ptr<RenderPass>> PassGroup::passes() const
{
    raw_ptr<RenderEngine> engine = engine_.get();
    if (engine == nullptr) {
        return {};
    }
    return engine->groupPasses(id_);
}

raw_ptr<RenderPass> PassGroup::pass(const String& role) const
{
    raw_ptr<RenderEngine> engine = engine_.get();
    if (engine == nullptr) {
        return nullptr;
    }
    return engine->groupPass(id_, role);
}

VN_GRAPHICS_NS_END
