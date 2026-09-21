#pragma once

#include <cstdint>

#include <vine/vsg/core/Keys.hpp>
#include <vine/vsg/core/VariantPool.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief One pass' state: which variant is bound, and what has to be re-issued before the next draw.
 *
 * WHY IT IS PER PASS. The API layer compiles a pipeline per view (vsg compiles a `GraphicsPipeline` per
 * viewID), so "the state registry" cannot be one global object: each pass has its own command sequence, its
 * own currently bound variant, and its own recorded dynamic values. What the passes SHARE is the compiled
 * variant itself, which is why the pool is passed in rather than owned.
 *
 * WHAT IT DECIDES, AND WHAT IT ONLY COUNTS. Three questions, all cheap: is this pipeline identity already
 * compiled (the pool answers), does the dynamic block have to be issued before the next draw (this object
 * answers by comparing with what it last issued), and has this pass already bound the sampled-input set
 * (answered by the set's identity - a pass' inputs do not change while it records, so once per pass is the
 * true cost). It never decides rebuild/resize/borrow - those belong to the frame's compiler - and it never
 * touches a GPU object.
 *
 * THE POINT IS THE ARITHMETIC. A host that changes cull, polygon, blend, depth policy or topology churns the
 * DYNAMIC half: that is N set commands and ONE compile, no matter how often the value changes (the identity
 * never moved). A host that swaps a program or changes a vertex layout churns the IDENTITY half: that is one
 * more variant. The counters - pool.created(), this object's dynamic_issued() and its inputs_issued() - are
 * how a phase tells the three apart after the fact, instead of trusting a comment.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
V_VSG_NS_BEGIN

namespace core
{

/** @brief The state of one pass: variant selection plus the dynamic block's issued values. */
class StateRegistry
{
  public:
    /** @brief What a draw has to do with the state, given what this registry last answered. */
    struct Resolution
    {
        std::uint64_t variant{0};            ///< The variant to bind.
        bool          variant_switched{false};  ///< A different variant than the last one resolved here.
        bool          dynamic_issued{false};    ///< The dynamic block has to be issued before drawing.
        bool          inputs_issued{false};     ///< The sampled-input set has to be bound before drawing.
    };

  public:
    /** @brief Constructs a registry for one pass.
     *
     * @param pool The scope's variant pool (shared by every pass of the scope; the registry does not own it).
     */
    explicit StateRegistry(VariantPool& pool) noexcept;

    StateRegistry(const StateRegistry&) = delete;
    StateRegistry& operator=(const StateRegistry&) = delete;

  public:
    /** @brief Resolves the state a draw needs: the variant for @p key, and whether @p state must be issued.
     *
     * @param key    Pipeline identity - the compile half (see the file note).
     * @param state  Dynamic state - the set-command half. It never reaches the pool.
     * @param inputs Identity of the sampled-input set this draw binds, or nullptr when the pass has none.
     *               An OPAQUE identity on purpose: the registry is core and must not know what a descriptor
     *               set is, only that "the pass already bound this one" is what makes the second bind
     *               unnecessary. The set is a property of the PASS (its inputs), so one bind per pass is the
     *               right cost - not one per draw.
     * @return The variant and the three costs (see @ref Resolution).
     */
    [[nodiscard]] Resolution resolve(const PipelineKey& key, const DynamicState& state,
                                     const void* inputs = nullptr);

    /** @brief Forgets what is bound, so the next resolution issues everything again.
     *
     * Called when the pass' recording starts over (a new command buffer) - the values a previous recording
     * issued do not exist in the new one.
     */
    void reset() noexcept;

  public:
    /** @brief Gets the variant the last resolution answered (0 when nothing was resolved since the reset). */
    [[nodiscard]] std::uint64_t currentVariant() const noexcept;

    /** @brief Gets the dynamic state the last resolution issued. */
    [[nodiscard]] const DynamicState& currentState() const noexcept;

    /** @brief Gets the number of resolutions. */
    [[nodiscard]] std::uint64_t resolutions() const noexcept;

    /** @brief Gets the number of resolutions that switched the pass' variant. */
    [[nodiscard]] std::uint64_t variant_switches() const noexcept;

    /** @brief Gets the number of resolutions whose dynamic block had to be issued. */
    [[nodiscard]] std::uint64_t dynamic_issued() const noexcept;

    /** @brief Gets the number of resolutions whose dynamic block was already current. */
    [[nodiscard]] std::uint64_t dynamic_skipped() const noexcept;

    /** @brief Gets the number of resolutions whose sampled-input set had to be bound. */
    [[nodiscard]] std::uint64_t inputs_issued() const noexcept;

    /** @brief Gets the number of resolutions whose sampled-input set was already the one bound. */
    [[nodiscard]] std::uint64_t inputs_skipped() const noexcept;

  private:
    VariantPool*  pool_;
    bool          bound_{false};
    std::uint64_t current_variant_{0};
    DynamicState  current_state_{};
    const void*   current_inputs_{nullptr};  ///< The sampled-input set the pass already bound (null = none yet).
    std::uint64_t resolutions_{0};
    std::uint64_t variant_switches_{0};
    std::uint64_t dynamic_issued_{0};
    std::uint64_t dynamic_skipped_{0};
    std::uint64_t inputs_issued_{0};
    std::uint64_t inputs_skipped_{0};
};

}  // namespace core

V_VSG_NS_END
