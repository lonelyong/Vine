#pragma once
#include "graphics_global.hpp"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "Camera.hpp"
#include "FrameDescription.hpp"
#include "ImageSource.hpp"
#include "Light.hpp"
#include "RenderCommand.hpp"
#include "RenderTarget.hpp"
#include "ShaderProgram.hpp"

VN_GRAPHICS_NS_BEGIN

class RenderPass;

/**
 * @brief One drawing call of an OWNED frame: the borrowed description's fields, each one held.
 *
 * @details
 * - The owned twin of @ref DrawDescription. Every handle is an owning one, so the value outlives the scene, the
 *   camera and the program it was assembled from - which is what lets a caller build a frame on one thread, keep it,
 *   and hand it to an executor later (or serialise it, or record it). The commands are a `shared_ptr` in both forms:
 *   one collection is usually drawn by several passes, and ownership of it is already shared.
 *
 * @note
 * - Like ClearPolicy, this is a header-only value: every member is defined here, so there is no symbol to export and
 *   the type deliberately carries no VN_GRAPHICS_API (the DLL never instantiates it).
 */
struct OwnedDraw
{
    /// Which drawing call this is (see DrawDescription::Kind).
    DrawDescription::Kind kind{ DrawDescription::Kind::Scene };
    /// The view this draw is projected and lit with (held; null means the draw issues nothing).
    intrusive_ptr<const Camera> camera;
    /// Scene kind: the commands this draw issues (held; the collection owns its geometry and materials).
    std::shared_ptr<const std::vector<RenderCommand>> commands;
    /// Screen kind: the target whose colour attachments the program samples (held).
    intrusive_ptr<RenderTarget> source;
    /// Screen kind: the fragment program to draw the source through (held).
    intrusive_ptr<const ShaderProgram> program;
    /// The lights this draw announces, or nullopt for NONE (see DrawDescription::lights).
    std::optional<std::vector<intrusive_ptr<const Light>>> lights;
};

/**
 * @brief What ONE pass of an owned frame issues: the borrowed @ref PassDescription with owning handles.
 *
 * @details
 * - Same shape, different ownership: `target` is held rather than borrowed, and the draws are @ref OwnedDraw. The
 *   fields that are plain values (viewport, clear, state) are copied verbatim, because a value cannot dangle.
 */
struct OwnedPassDescription
{
    /// The off-screen target this pass draws into (held; null = the presenting surface).
    intrusive_ptr<RenderTarget> target;
    /// The sub-rectangle it draws into, or nullopt for the whole of it.
    std::optional<Viewport> viewport;
    /// The clear it announces, or nullopt for none (see ClearPolicy).
    std::optional<ClearPolicy> clear;
    /// This pass' state for the content it draws.
    PassRenderState state{};
    /// The drawing calls, in the order they happen.
    std::vector<OwnedDraw> draws;
};

/**
 * @brief One pass of an OWNED frame: the engine's facts about it and what it issues, all held.
 *
 * @details
 * - The owned twin of @ref FrameDescription::Pass. `pass` is held (a backend may key retained per-pass state by
 *   it, and a recorded frame must not lose the identity it names), the inputs are the OWNING declarations
 *   (@ref ImageInput), and the description is @ref OwnedPassDescription.
 * - The inputs are declarations rather than resolved sources on purpose: resolving is what the engine does for a
 *   frame it is about to run, and an owned frame states what the host declared. @ref OwnedFrame::borrowed resolves
 *   them the same way a compiled frame carries them.
 */
struct OwnedPass
{
    /// The pass (its identity; held).
    intrusive_ptr<const RenderPass> pass;
    /// Where the pass stacks in its target (see FrameDescription::Pass::order).
    int order{ 0 };
    /// What its inputs are, in declaration order (owning declarations).
    std::vector<ImageInput> inputs;
    /// What the pass issues, or nullopt when it issues nothing at all.
    std::optional<OwnedPassDescription> description;
};

/**
 * @brief ONE FRAME as an OWNED value: what a caller keeps when the producer's objects may go away.
 *
 * @details
 * - WHY IT EXISTS. `FrameDescription` is an EPHEMERAL, BORROWED snapshot: its pointers are valid only until the next
 *   compile, or for the duration of one `executeFrame()` call, and the executor contract says a frame is driven from
 *   the thread that owns the device. That shape is right for the engine's own path - it costs no reference counting
 *   per frame - and wrong for the three cases where a frame must outlive its producers: offline rendering, remote
 *   rendering (a worker thread or another process), and frame recording/replay. This is those cases' value.
 * - WHAT IT OWNS: the passes, their targets, the cameras and programs their draws name, the command collections (whose
 *   commands own their geometry and materials), the lights, and the input declarations. Nothing in it points at
 *   something it does not keep alive.
 * - HOW IT IS CONSUMED: @ref borrowed() hands the backend-facing value over. The result is valid exactly as long as
 *   the owned frame is alive, which is the caller's to guarantee - and it is the SAME shape the engine compiles, so a
 *   backend needs no second path (see RenderFrameExecutor::executeFrame).
 * - WHAT IT IS NOT: a command buffer, a serialised format, or a second way to compile. Building one is the caller's
 *   (or an offline producer's) work; the engine keeps compiling borrowed frames, because that is the cheap path and
 *   paying reference-count churn per frame is what the borrowed form exists to avoid.
 * - WHY THERE IS NO `RenderEngine` ENTRY. An owned frame needs owning handles, and an engine's frame is built from
 *   values the HOST owns (its passes, targets, cameras, programs) - the party that can hand over owning handles is
 *   the one that already has them. An engine-built owned frame would have to retain a second copy of every handle
 *   per compile, which is exactly the per-frame cost the borrowed form avoids, and it would be a second compile
 *   path over the same slots. A RECORDING feature (which wants frames after the host let go) is a different design
 *   and starts from its own producer, not from this value.
 * - SERIALISATION is likewise not here: a format needs a consumer (a file, a wire, a replay), and none exists -
 *   the value is what such a consumer would serialise.
 */
class OwnedFrame
{
  public:
    /** @brief The passes of this frame, in the order they were announced (see FrameDescription::Pass). */
    std::vector<OwnedPass> passes;

    /**
     * @brief The borrowed frame value this owned frame describes.
     *
     * @details
     * - Every pointer in the result addresses storage this frame holds, so the result is valid while this frame is
     *   alive and must not outlive it. The command collections are shared, not copied (both forms use the same
     *   `shared_ptr`), and a pass with no description stays an entry with no description - an empty scope and an
     *   absent one remain different facts.
     * - Input declarations are RESOLVED here the way the engine resolves them: a produced image keeps its target,
     *   attachment and kind; an imported texture carries the revision its upload is built from and its declared
     *   sampler state.
     *
     * @return The frame to hand to RenderFrameExecutor::executeFrame.
     */
    [[nodiscard]] FrameDescription borrowed() const
    {
        FrameDescription frame;
        frame.passes.reserve(passes.size());
        for (const OwnedPass& owned : passes)
        {
            FrameDescription::Pass entry;
            entry.pass  = owned.pass.get();
            entry.order = owned.order;
            entry.inputs.reserve(owned.inputs.size());
            for (const ImageInput& input : owned.inputs)
            {
                if (const ImageRef* image = input.image())
                {
                    entry.inputs.push_back(ImageSource::producedImage(
                        image->target(), image->attachment(), image->kind() == ImageRef::Kind::Depth));
                }
                else if (const Texture* texture = input.texture())
                {
                    entry.inputs.push_back(ImageSource::imported(texture, input.sampler()));
                }
                else
                {
                    entry.inputs.emplace_back();
                }
            }
            if (owned.description.has_value())
            {
                entry.description = borrow(*owned.description);
            }
            frame.passes.push_back(std::move(entry));
        }
        return frame;
    }

  private:
    /** @brief Borrows one owned pass description (see borrowed). */
    [[nodiscard]] static PassDescription borrow(const OwnedPassDescription& owned)
    {
        PassDescription out;
        out.target   = owned.target.get();
        out.viewport = owned.viewport;
        out.clear    = owned.clear;
        out.state    = owned.state;
        out.draws.reserve(owned.draws.size());
        for (const OwnedDraw& draw : owned.draws)
        {
            DrawDescription borrowed_draw;
            borrowed_draw.kind     = draw.kind;
            borrowed_draw.camera   = draw.camera.get();
            borrowed_draw.commands = draw.commands;
            borrowed_draw.source   = draw.source.get();
            borrowed_draw.program  = draw.program;
            if (draw.lights.has_value())
            {
                std::vector<raw_ptr<const Light>> lights;
                lights.reserve(draw.lights->size());
                for (const intrusive_ptr<const Light>& light : *draw.lights)
                {
                    lights.push_back(light.get());
                }
                borrowed_draw.lights = std::move(lights);
            }
            out.draws.push_back(std::move(borrowed_draw));
        }
        return out;
    }
};

VN_GRAPHICS_NS_END
