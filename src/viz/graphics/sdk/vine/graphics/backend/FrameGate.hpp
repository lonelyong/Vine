#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <thread>

#include <vine/graphics/backend/BackendHalfGuard.hpp>
#include <vine/graphics/graphics_global.hpp>

VN_GRAPHICSBACKEND_NS_BEGIN

/**
 * @brief One device's frame gate: "one frame at a time" as a fact that is CHECKED instead of assumed.
 *
 * @details
 * - WHAT IT ENFORCES. A backend's frame machinery is device-wide today - one command pool, one fence, one uniform
 *   ring, one descriptor pool (see `../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`, step 1) -
 *   so two threads entering `executeFrame` at once corrupt it SILENTLY. This gate serialises them: the second caller
 *   waits, which is what "a device drives one frame at a time" means once it is implemented rather than documented.
 * - WHAT IT REFUSES. A frame started from the thread that is ALREADY driving one is a RE-ENTRY (a `before_draw` hook
 *   driving another engine, a sink calling back into the device). Waiting there would deadlock the caller against
 *   itself, so the gate answers an EMPTY lease instead: the backend reports it once and answers `Refused`.
 * - WHAT IT IS NOT. It does not make a device's state thread-safe in general: host verbs (`setRenderTarget`,
 *   `releaseContent`, ...) stay between-frame calls, which is the host's contract. Step 2 of the ADR replaces this
 *   gate with per-session state; until then the gate is the honest implementation of the contract the headers state.
 * - WHY A LEASE AND NOT enter()/leave(). A frame has many early returns, and a forgotten `leave()` would deadlock the
 *   next frame instead of failing where the mistake is. The lease leaves the gate in its destructor, so "forgot to
 *   leave" cannot be expressed.
 */
class FrameGate
{
  public:
    /**
     * @brief The right to drive one frame: holding it is holding the gate, and its destructor releases it.
     *
     * @details
     * - AN EMPTY LEASE IS THE REFUSAL (see FrameGate::enter): it holds nothing, so destroying it does nothing.
     * - It is MOVE-ONLY and single-owner: one frame, one lease.
     */
    class Lease
    {
      public:
        /** @brief Builds an empty lease: the caller may not drive a frame. */
        Lease() noexcept = default;

        /**
         * @brief Takes over the gate @p other holds (it is left empty).
         *
         * @param other The lease to take from; it is empty when this returns.
         */
        Lease(Lease&& other) noexcept
          : gate_(other.gate_)
        {
            other.gate_ = nullptr;
        }

        Lease(const Lease&)            = delete;
        Lease& operator=(const Lease&) = delete;
        Lease& operator=(Lease&&)      = delete;

        /** @brief Leaves the gate when this lease held it. */
        ~Lease()
        {
            if (gate_ != nullptr) {
                gate_->leave();
            }
        }

        /** @brief Answers whether this lease holds the gate (false: the frame was refused). */
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return gate_ != nullptr;
        }

      private:
        friend class FrameGate;

        /**
         * @brief Takes the gate @p gate: called by FrameGate::enter, which has already acquired it.
         *
         * @param gate The gate this lease holds (never null: an empty lease is built by the default constructor).
         */
        explicit Lease(FrameGate* gate) noexcept
          : gate_(gate)
        {
        }

        FrameGate* gate_{ nullptr };
    };

    /** @brief Builds a free gate (no frame is being driven). */
    FrameGate() = default;

    /** @brief A gate destroyed while a frame holds it is a frame outliving its device (asserted in Debug). */
    ~FrameGate() noexcept
    {
        assert(owner_.load() == std::thread::id{} && "a frame lease outlived the device that owned the gate");
    }

    FrameGate(const FrameGate&)            = delete;
    FrameGate& operator=(const FrameGate&) = delete;
    FrameGate(FrameGate&&)                 = delete;
    FrameGate& operator=(FrameGate&&)      = delete;

    /**
     * @brief Takes the gate, waiting while another thread drives a frame.
     *
     * @details
     * - ANOTHER THREAD: waits. The wait IS the contract ("one frame at a time"), and it is short - a frame is one
     *   record, one submit and one fence wait, all inside the call.
     * - THIS THREAD: answers an EMPTY lease at once, because waiting would be the caller waiting for itself.
     *
     * @return The lease; empty when this thread already drives a frame on this device.
     */
    [[nodiscard]] Lease enter()
    {
        if (heldByThisThread()) {
            return Lease{};
        }
        mutex_.lock();
        owner_.store(std::this_thread::get_id());
        return Lease{ this };
    }

    /** @brief Answers whether the calling thread currently holds this gate. */
    [[nodiscard]] bool heldByThisThread() const noexcept
    {
        return owner_.load() == std::this_thread::get_id();
    }

  private:
    friend class Lease;

    /** @brief Releases the gate (called by the lease that holds it). */
    void leave() noexcept
    {
        assert(heldByThisThread() && "a frame lease was released by a thread that did not take it");
        owner_.store(std::thread::id{});
        mutex_.unlock();
    }

    /// Guards the gate itself; the frame runs while it is held, which is the point.
    std::mutex mutex_;
    /// Who holds it, read WITHOUT the lock so the re-entry question can be answered before waiting.
    std::atomic<std::thread::id> owner_{};
};

VN_GRAPHICSBACKEND_NS_END
