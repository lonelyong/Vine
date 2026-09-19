#include <vine/vsg/VsgGpuProfile.hpp>

#include <map>
#include <utility>

#include <vsg/app/RenderGraph.h>
#include <vsg/app/Window.h>
#include <vsg/utils/Profiler.h>
#include <vsg/vk/PhysicalDevice.h>

#include <vine/vsg/VsgRenderTargetEntry.hpp>
#include <vine/vsg/VsgRendererState.hpp>

V_VSG_NS_BEGIN

namespace detail
{

VsgGpuPassSample describeGraph(vine::graphics::RenderTarget* target, const VsgRenderTargetEntry& entry,
                               const ::vsg::RenderGraph* graph)
{
    VsgGpuPassSample sample;
    if (target == nullptr) {
        // The window target is the one whose graph is the shared swapchain graph: every window pass is a VIEW
        // of it, so there is no single pass to name (see VsgRenderTargetEntry::graph).
        sample.pass   = "window";
        sample.target = "window";
        return sample;
    }
    sample.target = target->name().empty() ? std::string("(unnamed)") : target->name().as_std_str();
    for (const auto& pass : entry.passes) {
        if (pass.second.graph.get() != graph) {
            continue;
        }
        const auto* owner = pass.first.owner;
        sample.pass       = (owner != nullptr && !owner->name().empty()) ? owner->name().as_std_str()
                                                                        : std::string("(unnamed)");
        sample.order      = pass.second.order;
        return sample;
    }
    // A graph the target's pass table no longer names: the pass was rebuilt between the measured frame and
    // this read. The interval is real, the pass behind it is not knowable any more.
    sample.pass = "(rebuilt)";
    return sample;
}

VsgGpuProfile readGpuProfile(const VsgRendererState& state)
{
    VsgGpuProfile profile;
    if (state.profiler == nullptr) {
        return profile;
    }
    profile.enabled = true;

    // The device's answer, asked of the device: a session that cannot timestamp has nothing to report, and
    // "no samples" must not be read as "nothing was on the GPU" (see VsgGpuProfile::timestamps_available).
    if (state.window == nullptr) {
        return profile;
    }
    const auto physical_device = state.window->getOrCreatePhysicalDevice();
    if (physical_device == nullptr) {
        return profile;
    }
    profile.timestamps_available = physical_device->getProperties().limits.timestampComputeAndGraphics != VK_FALSE;
    if (!profile.timestamps_available) {
        return profile;
    }

    const auto& log = state.profiler->log;
    if (log == nullptr) {
        return profile;
    }

    // Which pass a measured graph belongs to, taken from the LIVE session. A measured frame may name a graph
    // that has since been replaced or released, and the profiler's log holds RAW POINTERS into those nodes --
    // so an entry is only ever COMPARED against this table, never inspected: not by dereferencing it and not
    // by `dynamic_cast` either, which reads the object's vtable and crashed on exactly that (measured: a
    // released target's graph, freed a few frames later, was still named by an entry the reader walked past).
    // The address is the key; `std::less<const void*>` is what makes the comparison a total order rather
    // than whatever the built-in `<` does with unrelated pointers.
    std::map<const void*, VsgGpuPassSample, std::less<const void*>> measured;
    for (const auto& target_entry : state.targets) {
        const VsgRenderTargetEntry& entry = target_entry.second;
        if (entry.graph != nullptr) {
            measured.emplace(static_cast<const void*>(entry.graph.get()),
                             describeGraph(target_entry.first, entry, entry.graph.get()));
        }
        for (const auto& pass : entry.passes) {
            if (pass.second.graph != nullptr) {
                measured.emplace(static_cast<const void*>(pass.second.graph.get()),
                                 describeGraph(target_entry.first, entry, pass.second.graph.get()));
            }
        }
    }

    // The newest frame with results. The profiler reads a frame's queries several frames after it was
    // recorded (it reads without waiting, see the header), so the LAST frames in the log have no timestamps
    // yet -- walking the log backwards is what turns "not readable yet" into "look one frame further back".
    const auto& frames = log->frameIndices;
    for (std::size_t i = frames.size(); i-- > 0;) {
        const auto& frame_entry = log->entry(frames[i]);
        if (frame_entry.type != ::vsg::ProfileLog::FRAME || !frame_entry.enter) {
            continue; // the ring has already overwritten this frame's entry
        }
        const std::uint64_t frame_end = frame_entry.reference;
        if (frame_end <= frames[i] || (frame_end - frames[i]) >= log->entries.size()) {
            continue; // not a frame span this log still holds
        }

        VsgGpuProfile candidate;
        candidate.enabled              = true;
        candidate.timestamps_available = true;
        const double ticks_to_ms       = log->timestampScaleToMilliseconds;
        for (std::uint64_t reference = frames[i]; reference <= frame_end; ++reference) {
            const auto& interval = log->entry(reference);
            if (!interval.enter || interval.gpuTime == 0 || interval.reference <= reference ||
                interval.reference > frame_end) {
                continue;
            }
            const auto& end = log->entry(interval.reference);
            if (end.gpuTime <= interval.gpuTime) {
                continue; // this pair has not been read back yet
            }
            const double milliseconds = static_cast<double>(end.gpuTime - interval.gpuTime) * ticks_to_ms;
            if (interval.type == ::vsg::ProfileLog::COMMAND_BUFFER) {
                candidate.frame_gpu_ms = milliseconds;
                continue;
            }
            if (interval.type != ::vsg::ProfileLog::GPU) {
                continue;
            }
            const auto* graph    = static_cast<const void*>(interval.object);
            const auto  identity = measured.find(graph);
            if (identity == measured.end()) {
                continue; // an interval upstream's own hook wrote (no object), or a pass that is gone
            }
            VsgGpuPassSample sample = identity->second;
            sample.gpu_ms           = milliseconds;
            candidate.passes.push_back(sample);
        }
        if (!candidate.passes.empty()) {
            candidate.age_frames = frames.size() - 1u - i;
            return candidate;
        }
    }
    return profile;
}

} // namespace detail

V_VSG_NS_END
