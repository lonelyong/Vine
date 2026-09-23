#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameTimeline.hpp>
#include <vine/vsg/core/RetirementQueue.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The content tables' PRODUCTION SIDE: the live SDK objects a host tracks, turned into the tables a
 * frame's recording reads.
 *
 * WHY A PRODUCTION SIDE AT ALL. The tables a recorder reads (api/ContentFacts) answer questions no plan can
 * carry - a program's two texts and their declared bindings, a geometry's streams, a material's block bytes -
 * and until this class those entries were built by whoever drove the frame. A host that edits its scene does
 * not want to: a pipeline's ABI scan is work, and doing it per frame, per drawable, or at record time would
 * put content decisions inside the frame loop. So the store OWNS the live set and answers with the tables.
 *
 * WHY THE PLAN IS THE LIST. The frame's compiled plan names every identity the drawing needs, so the store
 * is asked for the tables OF that plan: what no plan names is not built (an object nothing draws needs no
 * entry), and what the plan names twice is built once. That is also the only list that can be trusted to be
 * complete - a list the host maintained by hand would be a second copy of the frame, and the two could
 * disagree in the direction that draws nothing.
 *
 * WHY THE PLAN'S REVISION IS NOT USED. A build never resurrects a revision: it describes the object AS IT IS
 * NOW (`Geometry::revision()`, `ShaderProgram::revision()`, and for a material the store's own counter, since
 * the SDK type has none). A plan compiled before an edit names a revision the object no longer has, and its
 * lookup MISSES - which is the designed answer (see api/ContentFacts: "a miss is a report, not a fallback"),
 * not something a store could paper over: the bytes it would have to describe are gone.
 *
 * WHY A SUPERSEDED ENTRY STAYS. The plan that named revision 3 may still be RECORDED after the object has
 * moved to 4 (the frame drive re-records), so a replaced revision must keep answering until the slots that
 * could still name it are past - the same window the executor gives a replaced GPU object, through the same
 * `core::RetirementQueue`. The entry is therefore not replaced but JOINED: the tables carry both revisions,
 * and the old one leaves when its park comes due. The one exception is the MATERIAL, whose lookup is by
 * identity alone (a plan cannot name a material revision - see findMaterial): the table has to answer "the
 * material now", so an edit REPLACES the entry in place and only the replaced value is parked.
 *
 * WHY AN ENTRY OWNS ITS OBJECT. The store is keyed by the object's ADDRESS and cannot observe its
 * destruction, so an entry that merely referenced it could outlive it - and a new object allocated at the
 * recycled address would be served the dead one's facts (wrong picture, silently). The store therefore holds
 * an owning reference per tracked object, exactly like api/MaterialImages does for its textures, and
 * @ref releaseAbandoned drops what nobody else holds.
 *
 * WHAT IT REFUSES TO DO. It reports nothing: an object it cannot describe (a geometry without positions, a
 * program that is not one content pipeline) simply gets no entry, and the recorder - which owns the
 * diagnostic stream and the per-episode reporting discipline - is where that becomes a message. That is also
 * what keeps this class testable without a device: building an entry needs no Vulkan object, only the SDK's
 * own accessors.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
V_VSG_NS_BEGIN

/** @brief The live content a host tracks, and the tables the frame's recording reads (see the file note). */
class ContentStore
{
  public:
    ContentStore();

    /**
     * @brief Takes (a share of) ownership of @p geometry and answers for it from now on.
     *
     * Tracking the same object again is not an error: the entry it already has is kept and the new share is
     * released with it. The object must stay tracked for as long as any frame may name it.
     *
     * @param geometry Geometry the host draws with; null is ignored.
     */
    void track(const vine::intrusive_ptr<vine::graphics::Geometry>& geometry);

    /**
     * @brief Takes (a share of) ownership of @p program and answers for it from now on.
     *
     * @param program Program the host draws with; null is ignored.
     */
    void track(const vine::intrusive_ptr<vine::graphics::ShaderProgram>& program);

    /**
     * @brief Takes (a share of) ownership of @p material and answers for it from now on.
     *
     * @param material Material the host shades with; null is ignored (content without a material is answered
     *                 for by the default entry, which needs no tracking).
     */
    void track(const vine::intrusive_ptr<vine::graphics::Material>& material);

    /**
     * @brief Touches @p material: refreshes its entry when the values it carries NOW differ from the row's.
     *
     * WHY A TOUCH, AND WHY IT COMPARES. The SDK's `MaterialManager` is where an edit is announced, but an
     * engine does not hand one to a backend (`RenderBackend` carries no such entry point), so the only place
     * a live edit can be noticed is where the material is announced: every material a frame commands - which
     * is exactly what the implementation this backend replaces did (`VsgMaterialManager::updateMaterial` was
     * called once per distinct material per frame, and was itself the compare-and-write). What the material
     * says NOW is built into a scratch block and compared with the row the table answers for it; only a
     * difference moves the revision, so a steady frame compares and writes nothing while an edit lands on
     * the very next frame that draws with it. The next frame that draws with it re-reads the block and
     * REPLACES the entry (see the file note).
     *
     * An edit for a material the host never tracked is ignored: nothing draws with it, so nothing can be
     * asked about it.
     *
     * @param material Material to touch; null is ignored (the default entry describes itself).
     */
    void updateMaterial(vine::raw_ptr<vine::graphics::Material> material);

    /**
     * @brief Ensures the tables answer for everything @p frame names, and returns them.
     *
     * The walk is what decides the work: every content command names a geometry, a material and a program
     * (the compiler resolves "no program of its own" into the frame's default before the plan exists), and
     * every full-screen call names a program. For each identity the store builds the entry the object is at
     * NOW, unless it already has it - a steady frame builds nothing.
     *
     * A command's VARIANT (see api/ProgramVariant) is computed from the facts of the same walk (the material
     * it is shaded with, the geometry it draws), so the program entry the recorder asks for is the one its
     * own text declares - `abi` and all.
     *
     * REPLACED REVISIONS AND REPLACED MATERIALS ARE PARKED through @p retirement, dated against @p timeline
     * (the caller's clock, like every other park in this backend), and released when the window is past.
     *
     * @param frame      The compiled plan whose identities are ensured.
     * @param timeline   The frame clock parks are dated against.
     * @param retirement Where replaced entries are parked.
     * @return The tables, valid until the next call - they borrow this store's own storage.
     */
    [[nodiscard]] const ContentFacts& tablesFor(const core::CompiledFrame& frame, core::FrameTimeline& timeline,
                                                core::RetirementQueue& retirement);

    /**
     * @brief Drops the tracked objects nobody else holds any more, and parks their entries' removal.
     *
     * An object is abandoned when the store's share is the last one: nothing can draw with it again, so its
     * rows have no future lookups to answer - but they are parked rather than dropped, because a plan that
     * already named the object may still be recorded.
     *
     * @param timeline   The frame clock parks are dated against.
     * @param retirement Where the abandoned entries' removals are parked.
     * @return Number of abandoned objects released.
     */
    std::size_t releaseAbandoned(core::FrameTimeline& timeline, core::RetirementQueue& retirement);

    /** @brief Forgets every tracked object and every entry. The session-teardown path: nothing is parked. */
    void clear();

    /** @brief Gets how many geometry entries the tables carry (one per identity and revision). */
    [[nodiscard]] std::size_t geometryEntries() const noexcept;

    /** @brief Gets how many program entries the tables carry (one per identity, revision, variant and kind). */
    [[nodiscard]] std::size_t programEntries() const noexcept;

    /** @brief Gets how many material entries the tables carry (one per identity). */
    [[nodiscard]] std::size_t materialEntries() const noexcept;

    /** @brief Gets how many entries were built in total: a steady frame raises this by nothing. */
    [[nodiscard]] std::uint64_t builds() const noexcept;

    /**
     * @brief Gets how many supersessions could not be parked and were retained instead.
     *
     * A store that is never told how many frames may be in flight cannot park, and the honest answer to "we
     * do not know" is to keep the entry (it costs memory, never correctness) rather than to free it while a
     * recorded frame may still name it. The count says how many supersessions took that path.
     */
    [[nodiscard]] std::size_t retained() const noexcept;

    ~ContentStore();

    ContentStore(const ContentStore&)            = delete;
    ContentStore& operator=(const ContentStore&) = delete;

  private:
    /** @brief Ensures the tables answer for one named geometry (see tablesFor). */
    void ensureGeometry(const vine::graphics::Geometry* geometry, core::FrameTimeline& timeline,
                        core::RetirementQueue& retirement);

    /** @brief Ensures the tables answer for one named material (see tablesFor). */
    void ensureMaterial(const vine::graphics::Material* material, core::FrameTimeline& timeline,
                        core::RetirementQueue& retirement);

    /** @brief Ensures the tables answer for one named program IN ONE GEOMETRY-FREE KIND (see tablesFor). */
    void ensureProgram(const vine::graphics::ShaderProgram* program, const ProgramVariant& variant, bool screen,
                       core::FrameTimeline& timeline, core::RetirementQueue& retirement);

    struct Data;
    // A shared Data rather than a unique one: a parked removal holds a WEAK reference to it, so a store
    // that dies before its parks come due releases nothing and dangles nowhere.
    std::shared_ptr<Data> d;
};

V_VSG_NS_END
