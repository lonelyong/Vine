#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>

#include "ContentId.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Kind of a shader stage.
 */
enum class ShaderStageType
{
    Vertex,   // Vertex shader.
    Fragment, // Fragment (pixel) shader.
    Compute,  // Compute shader (reserved).
};

/**
 * @brief One shader stage: a GLSL source with an entry point.
 *
 * @details
 * - A value type aggregating the authored source of a single stage. The SDK is backend-agnostic and never
 *   compiles; the render backend compiles the GLSL to SPIR-V at run time (vsg ShaderCompiler, glslang) or
 *   consumes pre-compiled SPIR-V supplied through the program.
 */
struct VN_GRAPHICS_API ShaderStage {
    ShaderStageType type = ShaderStageType::Vertex; // Stage kind.
    String          source;                         // GLSL source for the stage.
    String          entryPoint = u8"main";          // Entry function name.
};

/**
 * @brief ONE program's text as an immutable VALUE: its identity, the revision it was read at, and its stages.
 *
 * @details
 * - THE PROGRAM'S HALF OF THE FRAME'S VALUE CHANNEL, the same shape as `GeometryDescription` and
 *   `MaterialDescription`: what crosses to a side that records the frame is this value, not the live program.
 * - IT NAMES ITSELF. The id travels with the text because a frame in flight keys its compiled variants by
 *   `(id, revision)` and cannot ask the object for either half on the thread that records (see ContentId.hpp).
 * - ONE PER REVISION: `ShaderProgram::description()` returns the cached instance for the program's current revision,
 *   so frames with no text edit share one object. A stage edit is a NEW value at the next publication boundary
 *   (see `Scene::beginEdit` and `.ai/design/graphics-threading.md`).
 * - `name` is diagnostics only (a program whose text is refused still has a name); it is taken with the text because
 *   the recorder no longer reads the object. A rename does not move the revision, so a cached description keeps the
 *   name it was built with - the same visibility the name had when the facts were described per revision.
 * - NOT thread-safe to mutate, and it never is: every field is set by the constructor and read-only afterwards.
 */
class VN_GRAPHICS_API ShaderProgramDescription final : public RefCounted<ShaderProgramDescription> {
  public:
    /**
     * @brief Constructs a description.
     *
     * @param id       The program identity the text belongs to.
     * @param revision The program revision the text was read at.
     * @param name     The program name at that revision (diagnostics only).
     * @param stages   The stages, in addition order.
     */
    ShaderProgramDescription(ProgramId id, std::uint64_t revision, String name, std::vector<ShaderStage> stages);

    /** @brief Gets the program identity. */
    [[nodiscard]] ProgramId id() const noexcept;

    /** @brief Gets the revision the text was read at. */
    [[nodiscard]] std::uint64_t revision() const noexcept;

    /** @brief Gets the program name at that revision (diagnostics only). */
    [[nodiscard]] const String& name() const noexcept;

    /** @brief Gets the stages, in addition order. */
    [[nodiscard]] std::span<const ShaderStage> stages() const noexcept;

  private:
    ProgramId                id_;
    std::uint64_t            revision_;
    String                   name_;
    std::vector<ShaderStage> stages_;
};

using ShaderProgramDescriptionPtr = intrusive_ptr<const ShaderProgramDescription>;

/**
 * @brief Backend-agnostic, user-authored shader program (a set of stages).
 *
 * @details
 * - The thin programmable-shading handle: it groups the GLSL sources of the stages (vertex + fragment
 *   today) that replace the engine's default program for the objects it is bound to. Parameters, texture
 *   slots and pass-level inputs are layered on later (see .ai/design/graphics-rendering.md).
 * - A Geometry whose program() is null uses the session's default content program (material driven) -
 *   zero regression.
 */
class VN_GRAPHICS_API ShaderProgram : public Object, public RefCounted<ShaderProgram> {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs an empty program (no stages). */
    ShaderProgram();
    ~ShaderProgram();

  public:
    /** @brief Gets this program's identity.
     *
     * @details
     * - ALLOCATED AT CONSTRUCTION, released with the object, so a table that outlived the program reads "gone"
     *   instead of resolving to whatever is built at the same address next (see ContentId.hpp).
     * - THE NAME IS NOT THE IDENTITY (see name()): two programs may share a name, and a rename keeps the identity.
     * - A program edit (addStage / clearStages / replaceStages / setStage) keeps the identity and moves
     *   `revision()`, so a frame in flight names the same program at the revision it recorded.
     *
     * @return The program id; never null for a constructed program.
     */
    [[nodiscard]] ProgramId id() const noexcept;

    /** @brief Gets the program name.
     *
     * @return The name (empty when never set).
     */
    String name() const;

    /** @brief Sets the program name.
     *
     * @param name New program name.
     */
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

    /** @brief Replaces all stages at once (bumps the content revision).
     *
     * @details
     * - The edit-path entry point for hot-reloading an authored shader: the backend keys its cached compiled
     *   ShaderSet / pipeline variants by this revision.
     *
     * @param stages New stage list.
     */
    void replaceStages(const std::vector<ShaderStage>& stages);

    /** @brief Replaces the stage at @p index (bumps the content revision).
     *
     * @details
     * - Lets an editor patch a single stage's source without rebuilding the stage list.
     *
     * @param index Index of the stage to replace.
     * @param stage Replacement stage.
     * @return true when @p index is in range and the stage was replaced.
     */
    bool setStage(std::size_t index, const ShaderStage& stage);

    /** @brief Gets the program's content revision.
     *
     * @details
     * - Bumped by every stage mutation (addStage / clearStages / replaceStages / setStage). Backends use it
     *   to detect that a retained program object's content changed, so compiled shader sets / pipeline
     *   variants built from the old content are rebuilt.
     *
     * @return Monotonic content revision (starts at 0).
     */
    std::uint64_t revision() const;

    /** @brief Gets the number of stages in the program.
     *
     * @return The stage count.
     */
    std::size_t stageCount() const;

    /** @brief Gets the stage at @p index.
     *
     * @param index Stage index.
     * @return Stage, or null when out of range.
     */
    const ShaderStage* stage(std::size_t index) const;

    /** @brief Gets all stages (read-only).
     *
     * @return The stages, in addition order.
     */
    const std::vector<ShaderStage>& stages() const;

    /**
     * @brief Gets the program's text as an immutable value, building it when the revision moved.
     *
     * @details
     * - THE CONVERSION, the same shape as `Geometry::description()` and `MaterialBase::description()`: call it where
     *   the program may still be read (the publish that copies a scene's rows, a pass describing itself). It caches,
     *   so the second call in a revision is an integer compare.
     * - What the RECORDING side reads is the value handed to it (`RenderCommand::program_description`,
     *   `PassDraw::program_override_description`), never this call: a program edit must reach a frame at a
     *   publication boundary, not in the middle of one (design graphics-threading.md).
     *
     * @return The cached description for the current revision.
     */
    [[nodiscard]] ShaderProgramDescriptionPtr description() const;

  private:
    ProgramId                id_;
    String                   name_;
    std::vector<ShaderStage> stages_;
    std::uint64_t            revision_ = 0;
    /// The text as a value, rebuilt only when revision_ moves (see description()).
    mutable ShaderProgramDescriptionPtr description_;
};

using ShaderProgramPtr = intrusive_ptr<ShaderProgram>;

VN_GRAPHICS_NS_END
