#include <vine/vsg/api/ContentFacts.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

V_VSG_NS_BEGIN

namespace
{

/// @brief Counts the bits set in @p mask (the canonical channels a layout declares).
std::uint32_t popcount(std::uint32_t mask) noexcept
{
    std::uint32_t count = 0;
    while (mask != 0U)
    {
        count += mask & 1U;
        mask >>= 1U;
    }
    return count;
}

}  // namespace

bool channelsMatchLayout(const GeometryFacts& facts) noexcept
{
    const std::uint32_t declared = popcount(facts.layout.canonical_mask) +
                                   static_cast<std::uint32_t>(facts.layout.custom_locations.size());
    return declared == facts.channels.size() && facts.indices.key.kind == core::StreamKind::Index;
}

bool blockFitsAbi(const MaterialFacts& facts) noexcept
{
    return facts.block.size() == sizeof(vine::graphics::VineMaterialBlock);
}

FactResult<ProgramFacts> findProgram(const ContentFacts& facts, const core::ProgramRef& program) noexcept
{
    if (program.program == nullptr)
    {
        // "No program of its own" is resolved to the frame's default by the compiler, so a null identity
        // reaching this lookup means the caller asked about a command that has no program AT ALL - which is
        // content the host did not shade, not content this table could answer for.
        return { nullptr, FactMiss::Unknown };
    }

    for (const ProgramFacts& entry : facts.programs)
    {
        if (entry.program != program.program)
        {
            continue;
        }
        if (entry.revision != program.revision)
        {
            return { nullptr, FactMiss::Revision };
        }
        return { &entry, FactMiss::None };
    }
    return { nullptr, FactMiss::Unknown };
}

FactResult<GeometryFacts> findGeometry(const ContentFacts& facts, const void* geometry,
                                       std::uint64_t revision) noexcept
{
    if (geometry == nullptr)
    {
        return { nullptr, FactMiss::Unknown };
    }

    for (const GeometryFacts& entry : facts.geometries)
    {
        if (entry.geometry != geometry)
        {
            continue;
        }
        if (entry.revision != revision)
        {
            return { nullptr, FactMiss::Revision };
        }
        if (!channelsMatchLayout(entry))
        {
            return { nullptr, FactMiss::Malformed };
        }
        return { &entry, FactMiss::None };
    }
    return { nullptr, FactMiss::Unknown };
}

FactResult<MaterialFacts> findMaterial(const ContentFacts& facts, const void* material) noexcept
{
    // A null identity is NOT special-cased: the engine's material is optional, and the default material is an
    // entry like any other (see api/ContentSources.hpp). A table that carries one answers for content without
    // a material; a table that does not says Unknown, and the content layer decides what that means.
    for (const MaterialFacts& entry : facts.materials)
    {
        if (entry.material != material)
        {
            continue;
        }
        if (!blockFitsAbi(entry))
        {
            return { nullptr, FactMiss::Malformed };
        }
        return { &entry, FactMiss::None };
    }
    return { nullptr, FactMiss::Unknown };
}

V_VSG_NS_END
