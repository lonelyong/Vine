#pragma once

#include <shared_mutex>

#include <vine/backend_vsg/backend_vsg_global.hpp>

/**
 * @brief One device's two-sided gate: FRAME BODIES share it, HOST VERBS own it, and neither waits for itself.
 *
 * @details
 * - WHAT IT REPLACES, AND WHY THE REPLACEMENT IS NOT A RENAME. `backend::FrameGate` answers "one frame at a
 *   time" for a device whose frame machinery is device-wide. On this backend the machinery a frame drives is
 *   NOT device-wide any more: the collection scratch belongs to the session that drives the frame (S8a), the
 *   executor's frame state belongs to the driver (S8b), and the device's caches are read by one *content phase*
 *   at a time behind the backend's content lock (S8c-1). What is left for one frame at a time is the SAME
 *   SESSION - and that is a per-window claim, not a device gate. So the device gate's job changes to the two
 *   rules that still hold for the device:
 *   * two frame bodies (possibly on two threads, through two drivers) may run AT ONCE - they take the gate in
 *     SHARED mode;
 *   * a host verb that reconfigures the device or reads a device-wide answer (initialize / shutdown / readback /
 *     the frame report / the window set) must not overlap ANY frame body - it takes the gate in EXCLUSIVE mode.
 * - WHAT IT REFUSES. The frame body of THIS thread, re-entered (a diagnostics sink or a hook driving another
 *   engine) gets an EMPTY body lease: waiting there would be the thread waiting for itself. The same is true in
 *   the host direction: a host verb called from a frame body's thread gets an EMPTY host lease, because the
 *   frame body holds the gate in shared mode and waiting for it to end would again be the thread waiting for
 *   itself. Which of the two an empty lease means is answered by @ref inFrameBody.
 * - WHAT A HOST VERB MUST DO WITH AN EMPTY LEASE: answer (a result value, or silence) and say nothing about
 *   the device. It must NOT try the gate again or fall through to the unlocked path - the refusal is the whole
 *   answer, and it is how "a sink called back into the device" stays a displayed protocol error instead of a
 *   deadlock.
 * - WHY A LEASE AND NOT enter()/leave(). A frame has many early returns (a foreign target, a session that was
 *   not up, a scratch that could not open), and a forgotten `leave()` would deadlock the next verb instead of
 *   failing where the mistake is. The lease releases in its destructor, so "forgot to leave" cannot be
 *   expressed.
 * - WHY THE DEPTH IS THREAD-LOCAL AND NOT ATOMIC. The question is "is THIS thread already inside a body?", which
 *   is per-thread by definition; an atomic shared by threads would answer the wrong question (one driver's body
 *   would mark every host verb as re-entered).
 *
 * @warning
 * - The gate orders FRAME BODIES against HOST VERBS only. It says nothing about two bodies touching the same
 *   device table - that is the content lock's job (see VsgBackend's frame drive), and about two frames driving
 *   the same window - that is the window's own claim.
 * - A gate object must outlive every lease it handed out (a lease outliving its device is a frame outliving its
 *   device, which is the contract's "the creator guarantees no frame in flight").
 */
VN_BACKEND_VSG_NS_BEGIN

class DeviceGate final
{
  public:
    /** @brief The right to drive a frame body: holding it is sharing the device, and its destructor gives it back. */
    class BodyLease
    {
      public:
        /** @brief Builds an empty lease: the caller may not drive a frame (it is already inside one). */
        BodyLease() noexcept = default;

        /**
         * @brief Takes over the lease @p other holds (it is left empty).
         *
         * @param other The lease to take from; it is empty when this returns.
         */
        BodyLease(BodyLease&& other) noexcept
          : gate_(other.gate_)
        {
            other.gate_ = nullptr;
        }

        BodyLease(const BodyLease&)            = delete;
        BodyLease& operator=(const BodyLease&) = delete;
        BodyLease& operator=(BodyLease&&)      = delete;

        /** @brief Leaves the shared side when this lease held it. */
        ~BodyLease()
        {
            if (gate_ != nullptr) {
                gate_->leaveBody();
            }
        }

        /** @brief Answers whether this lease holds the gate (false: the frame was refused). */
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return gate_ != nullptr;
        }

      private:
        friend class DeviceGate;

        /** @brief Takes the lease @p gate already acquired (called by DeviceGate::enterBody). */
        explicit BodyLease(DeviceGate* gate) noexcept
          : gate_(gate)
        {
        }

        DeviceGate* gate_{ nullptr };
    };

    /** @brief The right to reconfigure or inspect the device: holding it excludes every frame body. */
    class HostLease
    {
      public:
        /** @brief Builds an empty lease: the verb was called from inside a frame body and must answer, not wait. */
        HostLease() noexcept = default;

        /**
         * @brief Takes over the lease @p other holds (it is left empty).
         *
         * @param other The lease to take from; it is empty when this returns.
         */
        HostLease(HostLease&& other) noexcept
          : gate_(other.gate_)
        {
            other.gate_ = nullptr;
        }

        HostLease(const HostLease&)            = delete;
        HostLease& operator=(const HostLease&) = delete;
        HostLease& operator=(HostLease&&)      = delete;

        /** @brief Leaves the exclusive side when this lease held it. */
        ~HostLease()
        {
            if (gate_ != nullptr) {
                gate_->leaveHost();
            }
        }

        /** @brief Answers whether this lease holds the gate (false: the verb was called from inside a body). */
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return gate_ != nullptr;
        }

      private:
        friend class DeviceGate;

        /** @brief Takes the lease @p gate already acquired (called by DeviceGate::enterHost). */
        explicit HostLease(DeviceGate* gate) noexcept
          : gate_(gate)
        {
        }

        DeviceGate* gate_{ nullptr };
    };

  public:
    /** @brief Builds a free gate (no body and no host verb is inside). */
    DeviceGate() = default;

    DeviceGate(const DeviceGate&)            = delete;
    DeviceGate& operator=(const DeviceGate&) = delete;
    DeviceGate(DeviceGate&&)                 = delete;
    DeviceGate& operator=(DeviceGate&&)      = delete;

  public:
    /** @brief Enters a frame body: SHARED, and empty when THIS thread is already inside one.
     *
     * @details
     * - ANOTHER THREAD's body is fine: that is the point of the shared side - two drivers' frames overlap.
     * - THIS THREAD's body (a sink or a hook driving another engine) answers an EMPTY lease at once: waiting
     *   there would be the thread waiting for itself.
     *
     * @return The lease; empty when this thread already drives a frame on this device.
     */
    [[nodiscard]] BodyLease enterBody() noexcept
    {
        if (s_depth_ != 0) {
            return BodyLease{};
        }
        mutex_.lock_shared();
        s_depth_ = 1;
        return BodyLease{ this };
    }

    /** @brief Enters a host verb: EXCLUSIVE, and empty when THIS thread is inside a frame body.
     *
     * @details
     * - Waits while another thread's frame bodies run: that wait IS the contract (a device-wide answer cannot be
     *   read while a frame is changing it).
     * - Called from inside a frame body (a sink calling back into the device) answers an EMPTY lease at once, so
     *   the verb can answer a refusal instead of deadlocking the caller against itself (see @ref inFrameBody).
     *
     * @return The lease; empty when this thread is inside a frame body.
     */
    [[nodiscard]] HostLease enterHost() noexcept
    {
        if (s_depth_ != 0) {
            return HostLease{};
        }
        mutex_.lock();
        s_depth_ = -1;
        return HostLease{ this };
    }

    /** @brief Answers whether THIS thread is inside a frame body (the frame body of the empty host lease).
     *
     * @return true when the calling thread holds a body lease (a sink, a hook, a callback inside a frame).
     */
    [[nodiscard]] static bool inFrameBody() noexcept
    {
        return s_depth_ > 0;
    }

  private:
    /** @brief Gives the shared side back (from BodyLease). */
    void leaveBody() noexcept
    {
        s_depth_ = 0;
        mutex_.unlock_shared();
    }

    /** @brief Gives the exclusive side back (from HostLease). */
    void leaveHost() noexcept
    {
        s_depth_ = 0;
        mutex_.unlock();
    }

  private:
    std::shared_mutex mutex_{}; // SHARED: frame bodies; EXCLUSIVE: host verbs.
    /// This thread's side of the gate: +1 inside a frame body, -1 inside a host verb, 0 outside both.
    inline static thread_local int s_depth_{ 0 };
};

VN_BACKEND_VSG_NS_END
