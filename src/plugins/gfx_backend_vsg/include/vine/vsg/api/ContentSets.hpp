#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <vsg/core/ref_ptr.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/ContentImages.hpp>
#include <vine/vsg/api/ContentPass.hpp>
#include <vine/vsg/api/MaterialImages.hpp>
#include <vine/vsg/core/FrameTimeline.hpp>
#include <vine/vsg/core/RetirementQueue.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The DECLARED SETS a pass' drawables need, produced from the tables (see api/ContentPass for what a
 * declared set is).
 *
 * WHY IT EXISTS. Every set a program declares blocks in - and every set it declares sampled images in, except
 * the content path's input set, whose images are the pass' - is the CALLER's to build (see
 * api/ContentPipeline::declaredSets). Building one means answering three questions per declared sampler: where
 * does its image come from (the by-name policy of api/ContentImages), which texture revision is that (the
 * pair api/MaterialImages keys by, and the tag a pass picks a drawable's set by - see
 * BlockDescriptors::ImageSource), and when does the set it built stop being the right one. Every host would
 * answer them the same way, so they are answered here, once.
 *
 * WHERE EACH IMAGE COMES FROM:
 *   * `diffuseMap` (Material): the drawable's own texture, through @p images - a material with NO texture (or
 *     one whose texture cannot be used) takes the white fallback, the value the engine's own stages are
 *     written against ("no map" is white, not an unwritten binding).
 *   * `shadow_map` (Shadow): the map the pass' plan resolved (api/ContentImages::shadowImageOf), or the white
 *     stand-in - a 2D one, or a CUBE one when the text declares `samplerCube` (a 2D view where a cube is
 *     declared is an invalid descriptor, not an untextured draw).
 *   * every other name (Input): the pass' declared inputs, in the same order the pass' own input set binds
 *     them (input by input, each one's colour attachments and then its depth).
 *
 * WHAT A SET IS KEYED BY. The (program, revision, variant) its text came from, the set index, and the texture
 * revision its material images were resolved from: the first three say which declarations it serves (a new
 * revision is a new ABI, another text is another program), the last says whose images it carries - and two
 * drawables of ONE variant with different materials are exactly the case that needs it (their sets have the
 * same shape; see BlockDescriptors::ImageSource). A set whose samplers are all pass-level is tagged with an
 * EMPTY source, so every drawable of that variant shares it.
 *
 * LIFETIME. A set whose key the tables no longer answer (their own retirement took the revision away) can
 * never be asked for again, so it is parked through the same window every other replaced object gets. A
 * REFUSED set is remembered: rebuilding it every frame would fail the same way and say the same thing.
 *
 * WHAT IT DOES NOT DO. It does not build the pass' INPUT set (api/ContentPass does, from the key's counts),
 * it does not report - it counts its fallbacks and refusals, and the caller owns the diagnostic stream - and
 * it does not build a full-screen call's set: that one is the pass' own throughout (declaredSets is empty for
 * the screen ABI).
 *
 * The device and the storage outlive the producer (a repointed storage means a new producer: the sets it
 * built bind the buffer it was given). NOT thread-safe, like the rest of the backend.
 */
VN_VSG_NS_BEGIN

/** @brief Produces the declared sets a pass' drawables need (see the file note). */
class ContentSets
{
  public:
    /**
     * @brief Creates the producer.
     *
     * @param device  The device the sets belong to.
     * @param storage The frame's block storage the sets bind (its buffer).
     * @param images  The material images (the `diffuseMap` half of the by-name policy, plus the fallbacks).
     */
    ContentSets(::vsg::ref_ptr<::vsg::Device> device, const BlockStorage& storage, MaterialImages& images);

    /**
     * @brief Ensures the declared sets @p pass' drawables need, and returns them as candidates.
     *
     * The walk is the one api/ContentHalves and api/ContentPass make: each command's geometry (at the
     * revision the plan names), its material, the variant both imply, and the half that serves it - so a set
     * exists for exactly the (set index, image source) pairs a pass will ask for. A command the tables or
     * the halves cannot answer contributes nothing (the recorder refuses it first, and says why).
     *
     * Sets whose key the tables no longer answer are parked through @p retirement, dated against @p timeline.
     *
     * @param pass       The compiled pass whose draws the sets are for.
     * @param facts      The tables the identities are answered from.
     * @param halves     The halves the pass was given (api/ContentHalves' answer: their layers carry the
     *                   declared shapes and the input/depth samplers).
     * @param inputs     The images the pass' inputs offer (see ContentPass::record - nothing when it has
     *                   none).
     * @param timeline   The frame clock parks are dated against.
     * @param retirement Where the sets whose key left the tables are parked.
     * @return The candidate sets for `Scope::block_sets`, valid until the next call.
     */
    [[nodiscard]] std::span<BlockDescriptors* const> setsFor(const core::CompiledPass& pass,
                                                              const ContentFacts& facts,
                                                              std::span<const ContentPass::Scope::Entry> halves,
                                                              std::span<const InputImages> inputs,
                                                              core::FrameTimeline& timeline,
                                                              core::RetirementQueue& retirement);

    /** @brief Rebuilds every cached set over a replacement storage (what a grown budget produces).
     *
     * The binding shapes do not change, so only the sets' elements move: each cached set is repointed at
     * @p storage's buffer and keeps its layout - the pipelines compiled against that layout stay valid.
     * The set it REPLACES is parked through @p retirement for the same window every replaced object gets:
     * its VkDescriptorSet handle may still be named by a submitted command buffer, and the descriptor pool
     * would hand that handle back to the replacement - updating it then is `VUID-vkUpdateDescriptorSets-
     * None-03047` (measured: exactly this, before the park was added). Refused sets (a declaration this
     * backend cannot serve) stay refused: there is nothing to repoint, and a retry would answer the same
     * way.
     *
     * @param storage    The storage the sets bind from now on.
     * @param timeline   The frame clock the parks are dated against.
     * @param retirement Where the replaced sets are parked.
     * @return How many cached sets were repointed (0 when none was built yet - the sets built next bind
     *         @p storage anyway).
     */
    std::size_t repoint(const BlockStorage& storage, core::FrameTimeline& timeline,
                        core::RetirementQueue& retirement);

    /** @brief Gets how many sets are alive (a steady pass raises this by nothing). */
    [[nodiscard]] std::size_t sets() const noexcept;

    /** @brief Gets how many sets were built in total (a refused one counts once: it is not retried). */
    [[nodiscard]] std::uint64_t builds() const noexcept;

    /** @brief Gets how many material maps fell back to the white image (an unusable or absent texture). */
    [[nodiscard]] std::size_t fallbacks() const noexcept;

    /** @brief Gets how many sets were refused (a declaration this backend cannot serve). */
    [[nodiscard]] std::size_t refused() const noexcept;

    /** @brief Releases every set. The session-teardown path: nothing is parked. */
    void clear();

    ~ContentSets();

    ContentSets(const ContentSets&)            = delete;
    ContentSets& operator=(const ContentSets&) = delete;

  private:
    struct Data;
    // A shared Data rather than a unique one: a parked removal holds a WEAK reference to it, so a producer
    // that dies before its parks come due releases nothing and dangles nowhere.
    std::shared_ptr<Data> d;
};

VN_VSG_NS_END
