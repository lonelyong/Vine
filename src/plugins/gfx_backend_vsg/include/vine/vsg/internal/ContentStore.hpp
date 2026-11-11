#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/backend/FrameCompiler.hpp>
#include <vine/graphics/backend/FrameTimeline.hpp>
#include <vine/graphics/backend/RetirementQueue.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/vsg/internal/ContentFacts.hpp>
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
 * WHY AN ENTRY OWNS ITS SHAPE. The store is keyed by the IDENTITY a plan names and cannot observe the content's
 * destruction, so an entry that merely referenced the description it answers with could outlive it - and a later
 * geometry allocated at the recycled address would be served the dead one's facts (wrong picture, silently). The store
 * therefore holds an owning reference to the shape per tracked identity, exactly like api/MaterialImages does for its
 * textures, and @ref releaseAbandoned drops what nobody else holds. A geometry's own cache holds its current
 * description (see Geometry::description), so "nobody else holds it" means the geometry is gone or the revision moved
 * - the two cases whose rows are dead.
 *
 * WHAT IT REFUSES TO DO. It reports nothing: an object it cannot describe (a geometry without positions, a
 * program that is not one content pipeline) simply gets no entry, and the recorder - which owns the
 * diagnostic stream and the per-episode reporting discipline - is where that becomes a message. That is also
 * what keeps this class testable without a device: building an entry needs no Vulkan object, only the SDK's
 * own accessors.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
VN_VSG_NS_BEGIN

/** @brief The live content a host tracks, and the tables the frame's recording reads (see the file note). */
class ContentStore {
  public:
    ContentStore();

    /**
     * @brief Answers for the SHAPE @p description at @p identity from now on.
     *
     * WHY BOTH. The identity is the address the content side names (a node's, which a command carries and a lookup
     * matches); the description is the immutable value the shape was read into (see `Geometry::description`), so the
     * tables are built from values and never read content. A frame hands both, once per geometry it commands
     * (`RenderCommand::row` and `RenderCommand::description`).
     *
     * A NEW DESCRIPTION AT A KNOWN IDENTITY IS AN EDIT: the value replaces the old one, and the rows the tables answer
     * for the old revision stay answerable until the frames that may still record them are past - they leave at the
     * park `ensureGeometry` takes when it notices the move. Tracking the same pair again is not an error.
     *
     * @warning
     * - This overload does NOT hold the identity's address alive: it can only name the shape. The caller that drained
     *   the value keeps that address reserved (the row table does it for a mirrored node, see ADR-2026-11-09); a
     *   caller that also holds the object uses the overload below.
     *
     * @param identity    Address the plan names; null is ignored.
     * @param description Shape to describe; null is ignored (a frame that names no shape is reported as a miss).
     */
    void track(const void* identity, const vn::graphics::GeometryDescriptionPtr& description);

    /**
     * @brief Describes @p geometry and tracks that pair (the content-side convenience).
     *
     * A caller that still holds the authored object - a tool, a test, an inline host - names it and this converts it
     * once (`Geometry::description()` is cached per revision).
     *
     * @param geometry Geometry the host draws with; null is ignored.
     */
    void track(const vn::intrusive_ptr<vn::graphics::Geometry>& geometry);

    /**
     * @brief Takes (a share of) ownership of @p program and answers for it from now on.
     *
     * @param program Program the host draws with; null is ignored.
     */
    void track(const vn::intrusive_ptr<vn::graphics::ShaderProgram>& program);

    /**
     * @brief Takes (a share of) ownership of @p material and answers for it from now on.
     *
     * @param material Material the host shades with; null is ignored (content without a material is answered
     *                 for by the default entry, which needs no tracking).
     */
    void track(const vn::intrusive_ptr<vn::graphics::MaterialBase>& material);

    /**
     * @brief Answers for @p identity 's VALUES from now on - the material's half of the geometry overload above.
     *
     * @details
     * - THE VALUE, NOT THE OBJECT: a row is built from what the frame published (`RenderCommand::materialDescription`),
     *   so describing a material never reads the live material on the thread that records the frame. A material edit
     *   is a new value at the next publish, which is what makes an edit inside an edit batch land with the batch
     *   (design graphics-threading.md C18).
     * - The rows the tables answer for the old value stay answerable until the frames that may still record them are
     *   past, which `ensureMaterial` arranges when it notices the revision move (see `LiveMaterial::values`).
     * - A host that built its command by hand and never published a value is answered from the object's own cache
     *   (`MaterialBase::description`), the same door such a command uses everywhere else.
     *
     * @param identity Address the plan names; null is ignored.
     * @param values   The material's values; null is ignored (the object's own value answers instead).
     */
    void track(const vn::graphics::MaterialBase* identity, const vn::graphics::MaterialDescriptionPtr& values);

    /**
     * @brief Claims the right to report one of @p identity 's texture slots, once per episode.
     *
     * WHY IT IS HERE. A wrong slot (a cube map in a normal slot, a byte map in the wrong colour space) is a
     * fact about a MATERIAL, and the sentence belongs to the recorder - which is per frame (see
     * ContentPass). What has to outlive the frame is the memory of having said it, and the one long-lived
     * record keyed by material is this store's entry (graphics-material.md F11 / pitfall P82).
     *
     * THE STORE STILL REPORTS NOTHING (see the file note): this answers "is this the first time?" and records
     * it, the caller writes the sentence. An EPISODE ends when the material's values change - the entry is
     * re-armed with each new description - so a host that fixes one map and breaks another hears about the
     * second.
     *
     * @param identity Material identity, the pointer the facts carry (`MaterialFacts::material`).
     * @param slot     Slot index, in `vn::graphics::TextureSlot` order.
     * @return true when this episode has not reported that slot yet (an untracked material always answers
     *         true: there is no record to remember it in).
     */
    [[nodiscard]] bool claimSlotReport(const void* identity, std::size_t slot) noexcept;

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
    [[nodiscard]] const ContentFacts& tablesFor(const core::CompiledFrame& frame, core::FrameTimeline& timeline, core::RetirementQueue& retirement);

    /**
     * @brief Drops the tracked objects nobody else holds any more, and parks their entries' removal.
     *
     * An object is abandoned when the store's share is the last one: nothing can draw with it again, so its
     * rows have no future lookups to answer - but they are parked rather than dropped, because a plan that
     * already named the object may still be recorded.
     *
     * @param timeline           The frame clock parks are dated against.
     * @param retirement         Where the abandoned entries' removals are parked.
     * @param released_materials Receives the identities of abandoned MATERIALS (cleared first): the block
     *                           storage returns their arena slots. A raw pointer is only used as the arena's
     *                           key, never dereferenced, so it may already name freed memory.
     * @return Number of abandoned objects released.
     */
    std::size_t releaseAbandoned(core::FrameTimeline& timeline, core::RetirementQueue& retirement, std::vector<const void*>& released_materials);

    /**
     * @brief Marks the identity @p identity as done with and parks the removal of its rows.
     *
     * @details
     * - The live entry stays until the parked callback runs; that is what makes the release CANCELABLE:
     *   track() / ensureGeometry() clear the request and bump the token, so a later use keeps the identity
     *   resident and an old callback becomes a no-op.
     * - The rows leave when the in-flight window is past (see the retirement queue); the shape is untouched.
     * - When parking is unavailable the request is rolled back and false is returned: memory may be kept,
     *   never freed while a recorded frame can still name it.
     *
     * @param identity   Identity to release; null is refused.
     * @param timeline   Frame clock the park is dated against.
     * @param retirement Where the removal is parked.
     * @return true when the request was accepted (or the identity was not resident); false when it could not be
     *         parked safely.
     */
    bool releaseGeometry(const void* identity, core::FrameTimeline& timeline, core::RetirementQueue& retirement);

    /**
     * @brief Releases the identity @p geometry names (the content-side convenience).
     *
     * @param geometry   Geometry to release; null is refused.
     * @param timeline   Frame clock the park is dated against.
     * @param retirement Where the removal is parked.
     * @return true when the request was accepted (or nothing was resident); false when it could not be parked safely.
     */
    bool releaseGeometry(vn::raw_ptr<const vn::graphics::Geometry> geometry, core::FrameTimeline& timeline, core::RetirementQueue& retirement);

    /** @brief Marks a program the host still owns as releasable and parks the removal of its rows.
     *
     * @details
     * - Same token and parking semantics as releaseGeometry(): a later track() / ensureProgram() cancels
     *   the request, and a failed park leaves the store unchanged.
     *
     * @param program    Program to release; null is refused.
     * @param timeline   Frame clock the park is dated against.
     * @param retirement Where the removal is parked.
     * @return true when the request was accepted (or the object was not resident); false when it could not be
     *         parked safely.
     */
    bool releaseProgram(vn::raw_ptr<const vn::graphics::ShaderProgram> program, core::FrameTimeline& timeline, core::RetirementQueue& retirement);

    /** @brief Marks a material the host still owns as releasable and parks the removal of its row.
     *
     * @details
     * - Same token and parking semantics as releaseGeometry(): a later track() / updateMaterial() /
     *   ensureMaterial() cancels the request, and a failed park leaves the store unchanged.
     *
     * @param material   Material to release; null is refused.
     * @param timeline   Frame clock the park is dated against.
     * @param retirement Where the removal is parked.
     * @return true when the request was accepted (or the object was not resident); false when it could not be
     *         parked safely.
     */
    bool releaseMaterial(vn::raw_ptr<const vn::graphics::MaterialBase> material, core::FrameTimeline& timeline, core::RetirementQueue& retirement);

    /** @brief Forgets every tracked object and every entry. The session-teardown path: nothing is parked. */
    void clear();

    /** @brief Gets how many geometry entries the tables carry (one per identity and revision). */
    [[nodiscard]] std::size_t geometryEntries() const noexcept;

    /** @brief Gets how many program entries the tables carry (one per identity, revision, variant and kind). */
    [[nodiscard]] std::size_t programEntries() const noexcept;

    /** @brief Gets how many material entries the tables carry (one per identity). */
    [[nodiscard]] std::size_t materialEntries() const noexcept;

    /** @brief Gets how many material rows are LIVE (tracked objects, the default material excluded).
     *
     * This is the number the block storage sizes its material arena against: every row is a material the host
     * still holds, and the arena keeps a slot for each of them until the host releases it. O(1).
     */
    [[nodiscard]] std::size_t materialCount() const noexcept;

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
    /** @brief Ensures the tables answer for one named identity's current shape (see tablesFor). */
    void ensureGeometry(const void* identity, core::FrameTimeline& timeline, core::RetirementQueue& retirement);

    /** @brief Ensures the tables answer for one named material (see tablesFor). */
    void ensureMaterial(const vn::graphics::MaterialBase* material, core::FrameTimeline& timeline, core::RetirementQueue& retirement);

    /** @brief Ensures the tables answer for one named program IN ONE GEOMETRY-FREE KIND (see tablesFor). */
    void ensureProgram(const vn::graphics::ShaderProgram* program,
                       const ProgramVariant&              variant,
                       bool                               screen,
                       core::FrameTimeline&               timeline,
                       core::RetirementQueue&             retirement);

    struct Data;
    // A shared Data rather than a unique one: a parked removal holds a WEAK reference to it, so a store
    // that dies before its parks come due releases nothing and dangles nowhere.
    std::shared_ptr<Data> d;
};

VN_VSG_NS_END
