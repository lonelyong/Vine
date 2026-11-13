#pragma once

#include <cstdint>
#include <vector>

#include <vine/graphics/ContentId.hpp>
#include <vine/graphics/MaterialBase.hpp>
#include <vine/backend_vsg/internal/ContentFacts.hpp>
#include <vine/backend_vsg/internal/ContentSources.hpp>

namespace vn::test
{

/**
 * @brief A material identity a case can name without building an object.
 *
 * @details
 * - The tables key a material by `(id, revision)`, never by address, so a case that only needs DISTINCT identities needs
 *   distinct handles - this is the smallest way to say one.
 *
 * @param index Stand-in object number; it becomes the handle's slot index.
 * @return The id.
 */
inline vn::graphics::MaterialId materialId(int index)
{
    return vn::graphics::MaterialId(vn::graphics::Handle::make(static_cast<std::uint32_t>(index + 1), 1U, 0U));
}

/**
 * @brief The reference a plan would name for @p material at its current revision.
 *
 * @details
 * - THE SAME CONVERSION PRODUCTION MAKES (`FrameRecorder::snapshotCommands`): the identity is the material's own id and
 *   the revision is the one its values are at, so a case holding the object asks the question a frame would ask.
 *
 * @param material The material (or instance) to name.
 * @return The reference.
 */
inline vn::graphics::ContentRef materialRef(const vn::graphics::MaterialBase& material)
{
    return vn::graphics::ContentRef{ material.id().handle(), material.revision() };
}

/**
 * @brief `buildMaterialFacts` for a case that holds the object rather than a reference.
 *
 * @details
 * - A named conversion, once: the builder takes the `(id, revision)` pair the table is keyed by, and the revision is
 *   THE ONE THE VALUES ARE AT - a case passes a value, so the row is keyed by that value's revision (the same rule
 *   `ContentStore::ensureMaterial` follows). @p revision is the fallback for a case with no value at all (the default
 *   material), where there is no revision to read.
 *
 * @param material The material to describe, or null for the default material.
 * @param values   The values to pack, or null for the default material.
 * @param revision The revision to record when @p values names none.
 * @param out      Receives the entry (its block span points into @p storage).
 * @param storage  Caller-owned storage the entry borrows.
 * @return None when the entry was built.
 */
inline vn::backend_vsg::FactMiss buildMaterialFacts(const vn::graphics::MaterialBase*           material,
                                            const vn::graphics::MaterialDescriptionPtr& values,
                                            std::uint64_t                                revision,
                                            vn::backend_vsg::MaterialFacts&                      out,
                                            std::vector<std::byte>&                      storage)
{
    const vn::graphics::Handle    id           = material != nullptr ? material->id().handle() : vn::graphics::Handle{};
    const std::uint64_t           row_revision = values != nullptr ? values->revision() : revision;
    return vn::backend_vsg::buildMaterialFacts(vn::graphics::ContentRef{ id, row_revision }, values, out, storage);
}

} // namespace vn::test
