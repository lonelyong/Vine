#include <vine/vsg/api/ContentPass.hpp>

#include <array>
#include <cstddef>
#include <string>

#include <vine/vsg/api/DrawBlock.hpp>

V_VSG_NS_BEGIN

namespace
{

/// @brief Builds a `vine::String` from an ASCII sentence (the house spelling for UTF-8 bytes).
vine::String asString(const std::string& text)
{
    return vine::String(reinterpret_cast<const char8_t*>(text.c_str()));
}

/// @brief Names a table miss the way the message needs it.
const char* missText(FactMiss miss) noexcept
{
    switch (miss)
    {
    case FactMiss::None:
        return "none";
    case FactMiss::Unknown:
        return "the content layer was never told about this identity";
    case FactMiss::Revision:
        return "the table knows this identity at a different revision";
    case FactMiss::Malformed:
        return "the table's entry cannot be drawn (see api/ContentFacts.hpp)";
    }
    return "unknown";
}

/// @brief The bytes of one block, as the storage takes them.
template <typename Block>
std::span<const std::byte> bytesOf(const Block& block) noexcept
{
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(&block), sizeof(Block));
}

}  // namespace

ContentPass::ContentPass(const Scope& scope, core::Diagnostics& diagnostics) noexcept
    : scope_(scope)
    , diagnostics_(diagnostics)
{
}

bool ContentPass::record(const core::CompiledPass& pass, const ContentFacts& facts,
                         const core::RenderPassCompatibility& compatibility,
                         std::span<const std::byte> view_block, ::vsg::ref_ptr<::vsg::Node>& out)
{
    auto group = ::vsg::Group::create();

    // One view block per pass: it describes the view, and the pass has one camera.
    const BlockStorage::Block view = scope_.storage->writeView(view_block);
    if (!view.valid)
    {
        reportRefused("the pass' view block", "the frame's block budget is full");
        out = group;
        return false;
    }

    bool complete = true;
    for (const core::CompiledDraw& draw : pass.draws)
    {
        if (draw.kind != core::DrawKind::Content)
        {
            // A full-screen program drawing call drives the program-slot path, which this layer does not own.
            reportRefused("a full-screen drawing call", "the program-slot path is not wired yet");
            complete = false;
            continue;
        }
        for (const core::CompiledCommand& command : draw.commands)
        {
            if (!recordCommand(command, draw, pass, facts, compatibility, view.offset, *group))
            {
                complete = false;
            }
        }
    }

    out = group;
    return complete;
}

bool ContentPass::recordCommand(const core::CompiledCommand& command, const core::CompiledDraw& draw,
                                const core::CompiledPass& pass, const ContentFacts& facts,
                                const core::RenderPassCompatibility& compatibility, std::uint64_t view_offset,
                                ::vsg::Group& into)
{
    const FactResult<ProgramFacts> program = findProgram(facts, command.program);
    if (!program.found())
    {
        reportRefused("the command's program", program.miss);
        return false;
    }

    const FactResult<GeometryFacts> geometry = findGeometry(facts, command.geometry, command.geometry_revision);
    if (!geometry.found())
    {
        reportRefused("the command's geometry", geometry.miss);
        return false;
    }

    // The compiled half the pair names: one program's stages against one vertex layout (see the file note).
    // A half carries its own program and revision, so a command that names another program - or the same one at
    // a revision the half was not compiled from - is refused: borrowing a half's stages would draw a picture
    // nobody authored, and the pool would file it under the key the plan named.
    const Scope::Entry* entry         = nullptr;
    bool                program_known = false;
    for (const Scope::Entry& candidate : scope_.entries)
    {
        if (candidate.program != command.program.program || candidate.revision != command.program.revision)
        {
            continue;
        }
        program_known = true;
        if (candidate.layout == geometry.entry->layout)
        {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr)
    {
        // Which of the two did not match matters: the fixes are different ones (compile the program, or the
        // layout), and "not built for" alone would send the reader to the wrong half of the pipeline key.
        if (program_known)
        {
            reportRefused("the command's geometry", "its vertex layout is not one this pass was built for");
        }
        else
        {
            reportRefused("the command", "its program is not one this pass was built for (or not at that revision)");
        }
        return false;
    }

    const FactResult<MaterialFacts> material = findMaterial(facts, command.material);
    if (!material.found())
    {
        reportRefused("the command's material", material.miss);
        return false;
    }

    // The blocks: this draw's identity and data (the view's bytes came in with the pass).
    vine::graphics::VineDrawBlock draw_block;
    packDrawBlock(command, draw_block);
    const BlockStorage::Block block = scope_.storage->writeDraw(bytesOf(draw_block));
    const BlockStorage::MaterialWrite material_write =
        scope_.storage->writeMaterial(material.entry->material, material.entry->revision, material.entry->block);
    if (!block.valid)
    {
        reportRefused("the command's draw block", "the frame's block budget is full");
        return false;
    }

    // The streams: one bind per channel, in the entry's order (which is the binding order).
    std::array<::vsg::ref_ptr<::vsg::BindVertexBuffers>, kMaxChannels> binds;
    std::size_t                                                        bound = 0;
    for (const ChannelFacts& channel : geometry.entry->channels)
    {
        if (bound == binds.size())
        {
            reportRefused("the command's geometry", "it feeds more channels than this layer binds");
            return false;
        }
        const StreamUploads::VertexResult acquired = scope_.uploads->acquireVertex(channel.key, channel.data);
        if (acquired.bind == nullptr)
        {
            reportRefused("the command's geometry", "one of its channels could not be uploaded");
            return false;
        }
        binds[bound] = acquired.bind;
        ++bound;
    }

    const StreamUploads::IndexResult indices =
        scope_.uploads->acquireIndex(geometry.entry->indices.key, geometry.entry->indices.data);
    if (indices.bind == nullptr)
    {
        reportRefused("the command's geometry", "its index stream could not be uploaded");
        return false;
    }

    ContentDraw::Draw record;
    record.key.program            = program.entry->program;
    record.key.revision           = program.entry->revision;
    record.key.vertex_layout      = entry->layout;
    record.key.compatibility      = compatibility;
    record.key.depth_sampleable   = pass.depth_sampleable;
    record.key.sampled_color_count = 0U;  // the plan does not carry the pass' inputs yet (see the file note)
    record.dynamic                = command.dynamic;
    record.blocks                 = scope_.descriptors->bind(
        entry->pipelines->layout(), BlockDescriptors::Offsets{ view_offset, block.offset, material_write.offset });
    record.vertex_binds = std::span<const ::vsg::ref_ptr<::vsg::BindVertexBuffers>>(binds.data(), bound);
    record.index        = indices.bind;
    record.viewport     = ViewportRect{ static_cast<float>(draw.viewport.x), static_cast<float>(draw.viewport.y),
                                        static_cast<float>(draw.viewport.width),
                                        static_cast<float>(draw.viewport.height) };
    record.index_count       = geometry.entry->index_count;
    record.first_index       = geometry.entry->first_index;
    record.vertex_offset     = geometry.entry->vertex_offset;
    record.color_attachments = pass.color_attachments;

    ::vsg::ref_ptr<::vsg::Node> node = entry->draws->record(*scope_.registry, record);
    if (node == nullptr)
    {
        // The recorder refuses when the identity has no compiled pipeline: a state group without a pipeline bind
        // would draw with whatever was bound last.
        reportRefused("the command", "its pipeline identity has no compiled pipeline");
        return false;
    }
    into.addChild(node);
    return true;
}

void ContentPass::reportRefused(const char* what, FactMiss miss)
{
    const std::string message = std::string(what) + " is not drawn: " + missText(miss);
    diagnostics_.report(vine::graphics::DiagnosticSeverity::Warning,
                        vine::graphics::DiagnosticCategory::ContentSkipped, asString(message));
}

void ContentPass::reportRefused(const char* what, const char* why)
{
    const std::string message = std::string(what) + " is not drawn: " + why;
    diagnostics_.report(vine::graphics::DiagnosticSeverity::Warning,
                        vine::graphics::DiagnosticCategory::ContentSkipped, asString(message));
}

V_VSG_NS_END
