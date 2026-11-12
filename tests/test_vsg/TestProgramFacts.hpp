#pragma once

#include <cstdint>

#include <vine/graphics/ContentId.hpp>
#include <vine/graphics/ShaderProgram.hpp>

namespace vn::test
{

/**
 * @brief A program identity a case can name without building an object.
 *
 * @details
 * - The tables key a program by `(id, revision)`, never by address, so a case that only needs DISTINCT identities
 *   needs distinct handles - this is the smallest way to say one.
 *
 * @param index Stand-in object number; it becomes the handle's slot index.
 * @return The id.
 */
inline vn::graphics::ProgramId programId(int index)
{
    return vn::graphics::ProgramId(vn::graphics::Handle::make(static_cast<std::uint32_t>(index + 1), 1U, 0U));
}

/**
 * @brief The reference a plan names for a stand-in program at @p revision.
 *
 * @param index    Stand-in object number (see programId).
 * @param revision The revision the plan names.
 * @return The reference.
 */
inline vn::graphics::ContentRef programRef(int index, std::uint64_t revision)
{
    return vn::graphics::contentRefOf(programId(index), revision);
}

/**
 * @brief The reference a plan would name for @p program at its current revision.
 *
 * @details
 * - THE SAME CONVERSION PRODUCTION MAKES (`FrameRecorder::snapshotCommands`): the identity is the program's own
 *   id and the revision is the one its text is at, so a case holding the object asks the question a frame would
 *   ask (see `ShaderProgram::id()`).
 *
 * @param program The program to name.
 * @return The reference.
 */
inline vn::graphics::ContentRef programRef(const vn::graphics::ShaderProgram& program)
{
    return vn::graphics::contentRefOf(program.id(), program.revision());
}

} // namespace vn::test
