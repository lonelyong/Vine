#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/String.hpp>

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Kind of a shader stage.
 */
enum class ShaderStageType
{
    Vertex,   ///< Vertex shader.
    Fragment, ///< Fragment (pixel) shader.
    Compute,  ///< Compute shader (reserved).
};

/**
 * @brief One shader stage: a GLSL source with an entry point.
 *
 * A value type aggregating the authored source of a single stage. The SDK is
 * backend-agnostic and never compiles; the render backend compiles the GLSL
 * to SPIR-V at run time (vsg ShaderCompiler, glslang) or consumes
 * pre-compiled SPIR-V supplied through the program.
 */
struct VN_GRAPHICS_API ShaderStage
{
    ShaderStageType type = ShaderStageType::Vertex;  ///< Stage kind.
    String source;                                   ///< GLSL source for the stage.
    String entryPoint = u8"main";                    ///< Entry function name.
};

/**
 * @brief Backend-agnostic, user-authored shader program (a set of stages).
 *
 * ShaderProgram is the thin programmable-shading handle: it simply groups the
 * GLSL sources of the stages (vertex + fragment today) that replace the
 * engine's default program for the objects it is bound to. Parameters,
 * texture slots and pass-level inputs are layered on later (see
 * .ai/design/graphics-shader.md). A Geometry whose program() is null uses the
 * session's default content program (material driven) — zero regression.
 */
class VN_GRAPHICS_API ShaderProgram : public Object, public RefCounted<ShaderProgram> {
    VN_OBJECT_META_DECL;

  public:
    ShaderProgram();
    ~ShaderProgram();

  public:
    /** @brief Gets the program name. */
    String name() const;

    /** @brief Sets the program name. */
    void setName(const String& name);

    /** @brief Appends a shader stage.
     *
     * Bumps the content revision.
     *
     * @param stage Stage to add (its source is kept by value).
     */
    void addStage(const ShaderStage& stage);

    /** @brief Removes every stage.
     *
     * Bumps the content revision.
     */
    void clearStages();

    /** @brief Replaces all stages at once.
     *
     * Bumps the content revision. This is the edit-path entry point for
     * hot-reloading an authored shader (the backend keys its cached compiled
     * ShaderSet / pipeline variants by this revision).
     *
     * @param stages New stage list.
     */
    void replaceStages(const std::vector<ShaderStage>& stages);

    /** @brief Replaces the stage at @p index.
     *
     * Bumps the content revision. Lets an editor patch a single stage's source
     * without rebuilding the stage list.
     *
     * @param index Index of the stage to replace.
     * @param stage Replacement stage.
     * @return true when @p index is in range and the stage was replaced.
     */
    bool setStage(std::size_t index, const ShaderStage& stage);

    /** @brief Gets the program's content revision.
     *
     * Bumped by every stage mutation (addStage / clearStages / replaceStages /
     * setStage). Backends use it to detect that a retained program object's
     * content changed, so compiled shader sets / pipeline variants built from
     * the old content are rebuilt.
     *
     * @return Monotonic content revision (starts at 0).
     */
    std::uint64_t revision() const;

    /** @brief Gets the number of stages in the program. */
    std::size_t stageCount() const;

    /** @brief Gets the stage at @p index.
     *
     * @param index Stage index.
     * @return Stage, or null when out of range.
     */
    const ShaderStage* stage(std::size_t index) const;

    /** @brief Gets all stages (read-only). */
    const std::vector<ShaderStage>& stages() const;

  private:
    String name_;
    std::vector<ShaderStage> stages_;
    std::uint64_t revision_ = 0;
};

using ShaderProgramPtr = intrusive_ptr<ShaderProgram>;

VN_GRAPHICS_NS_END
