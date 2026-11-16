#pragma once

#include <utility>

#include <vine/graphics/Diagnostics.hpp>
#include <vine/graphics/RenderFrameExecutor.hpp>
#include <vine/backend_vsg/internal/VsgBackend.hpp>
#include <vine/backend_vsg/internal/VsgExecutor.hpp>
#include <vine/backend_vsg/backend_vsg_global.hpp>

/**
 * @brief ONE frame driver handed out by `VsgBackend::createSession()`: its own executor state over the device's pool.
 *
 * @details
 * - WHAT IT OWNS: one `VsgExecutor` - the frame state that is one frame's (the window it drives, the passes it
 *   recorded, its measurement entries, its skip count) and the default content program its engine declared. That is
 *   what makes two engines on one device possible at all: each calls THIS object, and a frame cannot see the other's
 *   frame state (`.ai/design/graphics-sessions.md` S8b).
 * - WHAT IT BORROWS: the device (`VsgBackend`), whose content world, target registry, clock and park queue are the
 *   pool every driver draws from. The device owns the driver (see `RenderDevice::createSession`) and destroys it in
 *   `releaseSession()` or `shutdown()`.
 * - It adds no policy: every verb forwards to the device's frame body with this driver's executor, so the picture and
 *   the reports are the ones the device produces for any driver. The device's OWN driver is the aggregate itself
 *   (`&executor()`), which is why this type is only made by `createSession()`.
 *
 * @warning One frame at a time per driver (the device's frame gate serialises frame bodies today): a second call from
 *          the same thread is refused, from another thread it waits - the same rule the aggregate states.
 */
VN_BACKEND_VSG_NS_BEGIN

class VN_BACKEND_VSG_API VsgDriver final : public vn::graphics::RenderFrameExecutor
{
  public:
    /**
     * @brief Builds a driver over @p device's pool.
     *
     * @param device      The device that owns this driver (borrowed; outlives it).
     * @param diagnostics The device's one diagnostic route (borrowed).
     * @param registry    The device's target table, shared by every driver (borrowed).
     */
    VsgDriver(VsgBackend& device, vn::graphics::Diagnostics& diagnostics, VsgTargetRegistry& registry) noexcept
      : device_(device)
      , executor_(diagnostics, registry)
    {
    }

    VsgDriver(const VsgDriver&)            = delete;
    VsgDriver& operator=(const VsgDriver&) = delete;

  public:
    /** @brief Runs a frame through this driver (see VsgBackend::executeFrameFor). */
    vn::graphics::PresentOutcome executeFrame(const vn::graphics::FrameDescription& frame,
                                              vn::graphics::PresentTarget*          present_to = nullptr) override
    {
        return device_.executeFrameFor(executor_, frame, present_to);
    }

    /** @brief Prepares a frame through this driver (see VsgBackend::prepareFrameFor). */
    [[nodiscard]] vn::graphics::PrepareReport prepareFrame(const vn::graphics::FrameDescription& frame,
                                                           vn::graphics::PresentTarget*          present_to = nullptr) override
    {
        return device_.prepareFrameFor(executor_, frame, present_to);
    }

    /** @brief Answers the device's report: the numbers describe the device, not one driver (see FrameReport). */
    [[nodiscard]] vn::graphics::FrameReport frameReport() const noexcept override
    {
        return device_.frameReport();
    }

    /** @brief Announces THIS driver's default content program (see RenderFrameExecutor::setDefaultContentProgram). */
    void setDefaultContentProgram(vn::intrusive_ptr<const vn::graphics::ShaderProgram> program) override
    {
        device_.adoptDefaultProgram(executor_, std::move(program));
    }

    /** @brief Gets this driver's executor state (the device's frame body and its tests read it). */
    [[nodiscard]] VsgExecutor& executor() noexcept
    {
        return executor_;
    }

  private:
    VsgBackend& device_;
    VsgExecutor executor_;
};

VN_BACKEND_VSG_NS_END
