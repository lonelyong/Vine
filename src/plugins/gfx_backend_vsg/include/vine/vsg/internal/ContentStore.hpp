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
 * WHY A SUPERSEDED ROW STAYS, AND HOW LONG. The plan that named revision 3 may still be RECORDED after the
 * object has moved to 4 (the frame drive re-records), so a replaced revision must keep answering for the
 * frames that still name it. So every row carries the EPOCH of the last plan that named it (the frame number),
 * and leaves when no live plan can name it any more - `last_epoch < oldest_live`, where `oldest_live` is the
 * first frame whose completion is not yet proven (`FrameTimeline::completedFrame() + 1`). The ONE row that
 * outlives that window is its entry's live answer: a drawable culled for a hundred frames is still the
 * content the host holds, so its facts (and the uploads they declare) stay. There is no per-entry park and no
 * release token: the row's age and its entry are the whole lifetime, and one pass at the frame boundary
 * reclaims what both agree is done (`ResidencyTable`'s rule, applied to rows).
 *
 * WHY AN ENTRY OWNS ITS CONTENT, AND FOR HOW LONG. The tables are keyed by the identity a plan names (a handle,
 * see ContentId.hpp), and a row is built from the OBJECT the host tracks - so the store holds an owning
 * reference per tracked identity, exactly like api/MaterialImages does for its textures. The entry must outlive
 * its rows: a material row borrows its maps from the material (`MaterialSlotFacts::texture`), so the share is
 * what keeps a row a plan may still name readable. The entry is therefore dropped when BOTH hold: the host is
 * done with it (@ref releaseGeometry / releaseProgram / releaseMaterial, or nobody else holds the object - see
 * @ref releaseAbandoned) AND no row of that identity is left. A geometry's own cache holds its current
 * description (see Geometry::description) for the same reason.
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
     * WHY BOTH. The identity is the draw-list row's node handle (see `RenderCommand::node`), which a plan names and a
     * lookup matches; the description is the immutable value the shape was read into (see `Geometry::description`), so
     * the tables are built from values and never read content. A frame hands both, once per geometry it commands
     * (`RenderCommand::node` and `RenderCommand::description`).
     *
     * A NEW DESCRIPTION AT A KNOWN IDENTITY IS AN EDIT: the value replaces the old one, and the rows the tables answer
     * for the old revision stay answerable until no live plan has named them for a window (see the file note).
     * Tracking the same pair again is not an error.
     *
     * @warning
     * - This overload does NOT hold the identity's object alive: it can only name the shape. The caller that drained
     *   the value keeps that object reserved; a caller that also holds the object uses the overload below.
     *
     * @param identity    Node id the plan names; a null id is ignored.
     * @param description Shape to describe; null is ignored (a frame that names no shape is reported as a miss).
     */
    void track(vn::graphics::NodeId identity, const vn::graphics::GeometryDescriptionPtr& description);

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
     * - The rows the tables answer for the old value stay answerable until no live plan has named them for a window
     *   (see `LiveMaterial::values` and the file note).
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
    [[nodiscard]] bool claimSlotReport(vn::graphics::MaterialId identity, std::size_t slot) noexcept;

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
     * REPLACED ROWS AGE OUT, AND SO DO THE ENTRIES THE HOST HAS LET GO. Every row this walk names is stamped with
     * @p timeline's frame; the walk first reclaims every row no live plan named for a window (`last_epoch <
     * completedFrame() + 1`) and then drops the entries whose host is done and whose rows are all gone.
     *
     * @param frame    The compiled plan whose identities are ensured.
     * @param timeline The frame clock the row epochs and the reclamation watermark come from.
     * @return The tables, valid until the next call - they borrow this store's own storage.
     */
    [[nodiscard]] const ContentFacts& tablesFor(const core::CompiledFrame& frame, const core::FrameTimeline& timeline);

    /**
     * @brief Drops the tracked objects nobody else holds any more.
     *
     * An object is abandoned when the store's share is the last one: nothing can draw with it again, so its
     * entry has no future to describe. The entry is marked released here and DROPPED when its rows are gone -
     * a plan already recorded may still name them (see the file note).
     *
     * @param released_materials Receives the identities of abandoned MATERIALS whose arena slots are free at
     *                           the next sweep (cleared first): the block storage returns those slots. A raw
     *                           pointer is only used as the arena's key, never dereferenced, so it may already
     *                           name freed memory.
     * @return Number of abandoned objects released.
     */
    std::size_t releaseAbandoned(std::vector<vn::graphics::MaterialId>& released_materials);

    /**
     * @brief Tells the store the host is done with @p identity, and drops its entry once its rows are gone.
     *
     * @details
     * - The release is a FLAG, not a parked callback: the entry stops describing (a later track() / ensureGeometry()
     *   that names it again clears the flag), and it leaves at the next frame boundary whose rows are all gone -
     *   the rows themselves age out by epoch (see the file note).
     * - A null identity is refused, and an identity that was never tracked is already released (true).
     * - Releasing a live identity twice is a no-op.
     *
     * @param identity Identity to release; a null id is refused.
     * @return true when the identity is not (or no longer) resident.
     */
    bool releaseGeometry(vn::graphics::NodeId identity);

    /**
     * @brief Releases the identity @p geometry names (the content-side convenience).
     *
     * @param geometry Geometry to release; null is refused.
     * @return true when the identity is not (or no longer) resident; false for a null geometry.
     */
    bool releaseGeometry(vn::raw_ptr<const vn::graphics::Geometry> geometry);

    /** @brief Tells the store the host is done with @p program, and drops its entry once its rows are gone.
     *
     * @details
     * - Same released-flag rule as releaseGeometry(): a later track() / ensureProgram() clears it, and the entry
     *   leaves when no row names it any more.
     *
     * @param program Program to release; null is refused.
     * @return true when the program is not (or no longer) resident; false for a null program.
     */
    bool releaseProgram(vn::raw_ptr<const vn::graphics::ShaderProgram> program);

    /** @brief Tells the store the host is done with @p material, and drops its entry once its rows are gone.
     *
     * @details
     * - Same released-flag rule as releaseGeometry(). The material's ARENA SLOT is not freed here: the row's block
     *   may still be read by a recorded plan, so the slot leaves through @ref releaseAbandoned's output once the row
     *   is gone (the sweep frees it then).
     *
     * @param material Material to release; null is refused.
     * @return true when the material is not (or no longer) resident; false for a null material.
     */
    bool releaseMaterial(vn::raw_ptr<const vn::graphics::MaterialBase> material);

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

    ~ContentStore();

    ContentStore(const ContentStore&)            = delete;
    ContentStore& operator=(const ContentStore&) = delete;

  private:
    /** @brief Ensures the tables answer for one named identity's current shape, stamped with @p epoch. */
    void ensureGeometry(vn::graphics::NodeId identity, std::uint64_t epoch);

    /** @brief Ensures the tables answer for one named material, stamped with @p epoch. */
    void ensureMaterial(vn::graphics::ContentRef material, std::uint64_t epoch);

    /** @brief Ensures the tables answer for one named program IN ONE GEOMETRY-FREE KIND (see tablesFor).
     *
     * The plan names the program by its identity (see `ShaderProgram::id()`); the revision is NOT taken from
     * the plan but read from the tracked object, so a build always describes the object AS IT IS NOW and a plan
     * that named a superseded revision looks up a row that is gone - which is the designed miss, not something
     * the store may paper over (see the class note).
     */
    void ensureProgram(vn::graphics::ProgramId program, const ProgramVariant& variant, bool screen, std::uint64_t epoch);

    struct Data;
    std::unique_ptr<Data> d;
};

VN_VSG_NS_END
