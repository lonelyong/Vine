#include <vine/vsg/api/ContentDraw.hpp>

V_VSG_NS_BEGIN

ContentDraw::ContentDraw(ContentPipeline& pipelines, core::VariantPool& pool,
                         detail::DynamicStateEntryPoints entry_points) noexcept
  : pipelines_(&pipelines), pool_(&pool), entry_points_(entry_points)
{
}

::vsg::ref_ptr<::vsg::StateGroup> ContentDraw::record(core::StateRegistry& registry, const Draw& draw)
{
    // The three "only when" answers come from the pass' registry: the variant bind only after a SWITCH, the
    // dynamic block only when a value differs from what this pass already issued, and the sampled-input set
    // only when this pass has not already bound that very set.
    const core::StateRegistry::Resolution resolution = registry.resolve(draw.key, draw.dynamic, draw.inputs.get());
    const ContentPipeline::Result       compiled     = pipelines_->acquire(*pool_, draw.key);
    if (compiled.pipeline == nullptr) {
        // Nothing is recorded: a group without a pipeline bind would draw with whatever was bound last.
        ++refusals_;
        return {};
    }

    auto group = ::vsg::StateGroup::create();
    if (resolution.variant_switched) {
        group->add(::vsg::BindGraphicsPipeline::create(compiled.pipeline));
        ++pipeline_binds_;
    }
    if (resolution.dynamic_issued) {
        group->add(makeDynamicStateCommand(draw.dynamic, draw.color_attachments, entry_points_));
        ++dynamic_commands_;
    }
    if (draw.blocks != nullptr) {
        group->add(draw.blocks);
    }
    if (draw.inputs != nullptr && resolution.inputs_issued) {
        group->add(draw.inputs);
        ++input_binds_;
    }

    auto commands = ::vsg::Commands::create();
    // The rectangle is a command, which is what keeps an extent out of the pipeline's identity: a resize
    // changes these two calls and nothing else.
    commands->addChild(makeViewportCommand(draw.viewport));
    commands->addChild(makeScissorCommand(draw.viewport));
    for (const ::vsg::ref_ptr<::vsg::BindVertexBuffers>& bind : draw.vertex_binds) {
        if (bind != nullptr) {
            commands->addChild(bind);
        }
    }
    if (draw.index != nullptr) {
        commands->addChild(draw.index);
    }
    commands->addChild(::vsg::DrawIndexed::create(draw.index_count, draw.instances, draw.first_index,
                                                  draw.vertex_offset, 0U));
    group->addChild(commands);

    ++draws_;
    return group;
}

::vsg::ref_ptr<::vsg::StateGroup> ContentDraw::recordScreen(core::StateRegistry& registry, const ScreenDraw& draw)
{
    // The same three "only when" answers a content draw gets, over the same identity arithmetic: a full-screen
    // draw is a different SHAPE, not a different bookkeeping. Its sampled set IS the whole state it binds, so
    // it plays the role the content path's inputs play (one bind per pass that samples).
    const core::StateRegistry::Resolution resolution = registry.resolve(draw.key, draw.dynamic, draw.samplers.get());
    const ContentPipeline::Result       compiled     = pipelines_->acquire(*pool_, draw.key);
    if (compiled.pipeline == nullptr) {
        // Nothing is recorded: a group without a pipeline bind would draw with whatever was bound last.
        ++refusals_;
        return {};
    }

    auto group = ::vsg::StateGroup::create();
    if (resolution.variant_switched) {
        group->add(::vsg::BindGraphicsPipeline::create(compiled.pipeline));
        ++pipeline_binds_;
    }
    if (resolution.dynamic_issued) {
        group->add(makeDynamicStateCommand(draw.dynamic, draw.color_attachments, entry_points_));
        ++dynamic_commands_;
    }
    if (draw.samplers != nullptr && resolution.inputs_issued) {
        group->add(draw.samplers);
        ++input_binds_;
    }

    auto commands = ::vsg::Commands::create();
    commands->addChild(makeViewportCommand(draw.viewport));
    commands->addChild(makeScissorCommand(draw.viewport));
    // Three vertices, generated from gl_VertexIndex by the full-screen vertex stage: no vertex buffer, no
    // index buffer, and the whole geometry is this one call.
    constexpr std::uint32_t kFullscreenVertices = 3U;
    commands->addChild(::vsg::Draw::create(kFullscreenVertices, 1U, 0U, 0U));
    group->addChild(commands);

    ++draws_;
    ++screen_draws_;
    return group;
}

std::uint64_t ContentDraw::draws() const noexcept
{
    return draws_;
}

std::uint64_t ContentDraw::screen_draws() const noexcept
{
    return screen_draws_;
}

std::uint64_t ContentDraw::pipeline_binds() const noexcept
{
    return pipeline_binds_;
}

std::uint64_t ContentDraw::dynamic_commands() const noexcept
{
    return dynamic_commands_;
}

std::uint64_t ContentDraw::input_binds() const noexcept
{
    return input_binds_;
}

std::uint64_t ContentDraw::refusals() const noexcept
{
    return refusals_;
}

V_VSG_NS_END
