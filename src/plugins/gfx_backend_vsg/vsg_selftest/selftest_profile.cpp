/**
 * @brief GPU profiling (R6): what the DEVICE spent per pass, and what proves the numbers are real.
 *
 * The backend could already time itself on the CPU (VsgBuildProfile: attach / compile / record / present).
 * This phase is about the other half -- the device side -- which upstream vsg has the machinery for
 * (vsg::Profiler writes timestamp queries per command buffer) and this backend now installs behind a switch
 * (VINE_VSG_PROFILE, see VsgGpuProfile.hpp).
 *
 * WHAT MAKES THIS A PHASE AND NOT A PRINT. Five claims, each with its own way to be false:
 *
 *   1. the measurement is LIVE: two passes, one drawing a single full-screen quad and one drawing the same
 *      quad 64 times with the depth test off, must come back with the heavy pass' GPU time LARGER than the
 *      light one's. A stub that reported zeros -- or a plausible-looking constant -- fails here, and so does
 *      a switch that never reached the profiler.
 *   2. it is ATTRIBUTED: the samples name the passes and targets that were announced, which is what the
 *      named wrapper nodes exist for (upstream's own per-graph timestamp carries no object, so nothing in
 *      the session could be looked up from it).
 *   3. it describes the frame it CLAIMS to: after the measurements, one measured target is released and
 *      another is created; the next profile must show the new one, drop the released one and keep the rest --
 *      so the samples are the NEWEST measured frame's recording, not stale entries lying in the log and not
 *      some older frame that still happens to hold results.
 *   4. it does not STALL: reading the results must not raise the session's device waits, and the frame the
 *      samples describe must lag the session (a profile that is never behind would be reading with
 *      VK_QUERY_RESULT_WAIT_BIT, which this backend must not do).
 *   5. the switch is a switch: the same driving in a session started WITHOUT VINE_VSG_PROFILE reports
 *      enabled == false and no samples at all, which is what makes every number above its doing.
 *
 * The phase runs its own session (shutdown + VINE_VSG_PROFILE + initialize) because the switch is read when
 * a session comes up, exactly like every other hatch this backend exposes.
 */

#include "selftest_support.hpp"

namespace selftest
{

namespace
{

/**
 * @brief How many full-screen quads the heavy pass draws (the light pass draws one).
 *
 * The two workloads have to differ by an ORDER OF MAGNITUDE, not by a few draws: on the software device a
 * render pass' own interval carries a fixed cost of about two milliseconds (its attachment load/store and the
 * queue's handling of it), which is what the light pass mostly measures too. Measured: with 64 draws the two
 * come out within noise of each other (x0.8), with 1024 the heavy pass is x3.7 the light one. A smaller
 * workload would make this phase assert on noise; a larger one would only make it slower.
 */
constexpr int kHeavyDraws = 1024;

/** @brief How many frames to drive before a measurement can be read back.
 *
 * The profiler reads a frame's queries several frames after it recorded them (it never waits), so a session
 * needs more frames than the read-back distance before anything is available at all.
 */
constexpr int kProfileFrames = 6;

/** @brief Sets or clears an environment variable the code under test reads with std::getenv().
 *
 * @param name Variable name.
 * @param on   true to set it to "1", false to clear it.
 */
void setEnvironmentFlag(const char* name, bool on)
{
#if defined(_WIN32)
    (void)::_putenv_s(name, on ? "1" : "");
#else
    if (on) {
        setenv(name, "1", 1);
    }
    else {
        unsetenv(name);
    }
#endif
}

/** @brief The sample of the pass whose target is @p target, or nullptr when it is not in the profile. */
const vine::vsg::VsgGpuPassSample* sampleForTarget(const vine::vsg::VsgGpuProfile& profile, const char* target)
{
    for (const auto& sample : profile.passes) {
        if (sample.target == target) {
            return &sample;
        }
    }
    return nullptr;
}

/** @brief One off-screen pass this phase measures: the target a sample is looked up by, and the pass. */
struct ProfilePass
{
    RenderTargetPtr target;
    RenderPassPtr   pass;
};

/** @brief Builds a 256x144 colour+depth target and the pass that renders into it.
 *
 * @param name Target and pass name (also the name the samples are matched against).
 * @return The pass, ready to be driven.
 */
ProfilePass makeProfilePass(const char8_t* name)
{
    ProfilePass built;
    built.target = RenderTargetPtr(new RenderTarget());
    built.pass   = RenderPassPtr(new RenderPass());
    built.target->setName(vine::String(name));
    built.target->setSize(256, 144);
    built.target->attachColor(RenderTarget::ColorFormat::RGBA8);
    built.target->attachDepth(RenderTarget::DepthFormat::D24);
    built.pass->setName(vine::String(name));
    return built;
}

/**
 * @brief Drives one pass INSIDE a frame the caller already opened.
 *
 * Split out from the frame loop because a measured frame drives several passes: `beginFrame()` opens the
 * frame, each pass announces itself and draws, and the frame is submitted once at the end -- which is how the
 * engine drives them too.
 *
 * The depth policy is Disabled on purpose: with the depth test on, the second and every following identical
 * quad is rejected before it shades anything, so 64 draws would cost what one costs and the phase would have
 * no signal to assert on.
 *
 * @param renderer Renderer under test.
 * @param built    Pass to drive.
 * @param commands Content to draw.
 * @param order    Pass order (the passes of one frame are driven in order).
 * @param camera   Camera to draw through.
 */
void driveProfilePassFrame(vine::vsg::VsgRenderer& renderer, const ProfilePass& built,
                           const std::vector<RenderCommand>& commands, int order, const CameraPtr& camera)
{
    const vine::Color clear_color(10, 20, 30, 255);
    PassScope         pass_scope(renderer, built.pass.get(), order, built.target.get(), clear_color, true,
                                 DepthMode::Disabled);
    renderer.render(commands, camera.get());
}

/** @brief Builds @p draws identical over-inking full-screen quads.
 *
 * @param draws How many commands to build.
 * @return The commands, all drawing the same geometry with the depth test off.
 */
std::vector<RenderCommand> profileQuads(int draws)
{
    const auto geometry = makeVisibleQuad(1.2f); // oversize: it covers the whole target
    const auto material = MaterialPtr(new Material());
    material->setDiffuse(vine::Colorf(0.4f, 0.6f, 0.9f, 1.0f));

    std::vector<RenderCommand> commands;
    commands.reserve(static_cast<std::size_t>(draws));
    for (int i = 0; i < draws; ++i) {
        commands.emplace_back(geometry, material, Mat4d());
    }
    return commands;
}

} // namespace

bool runGpuProfilePhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    (void)frames; // the phase drives its own count: a measurement needs more frames than the read-back lag

    renderer.shutdown();
    setEnvironmentFlag("VINE_VSG_PROFILE", true);
    if (!renderer.initialize()) {
        setEnvironmentFlag("VINE_VSG_PROFILE", false);
        std::fprintf(stderr, "[selftest] FAIL: a session with VINE_VSG_PROFILE could not come up\n");
        return false;
    }

    ProfilePass light = makeProfilePass(u8"profile-light");
    ProfilePass heavy = makeProfilePass(u8"profile-heavy");
    // The two workloads differ ONLY in how many times they draw: same geometry, same material, same target
    // size, same depth policy. Both passes are driven in the SAME frames, so the two samples the phase compares
    // come out of one measured frame -- a pass announces itself per frame, and a pass that is not announced is
    // retired and not recorded at all (which is why driving them in separate frames would compare a frame's
    // heavy pass against ANOTHER frame's light one).
    const auto light_commands = profileQuads(1);
    const auto heavy_commands = profileQuads(kHeavyDraws);

    for (int frame = 0; frame < kProfileFrames; ++frame) {
        FrameScope scope(renderer);
        driveProfilePassFrame(renderer, light, light_commands, 0, camera);
        driveProfilePassFrame(renderer, heavy, heavy_commands, 1, camera);
    }

    const std::size_t waits_before_reading = renderer.deviceWaitCount();
    const auto        measured             = renderer.gpuProfile();
    const std::size_t waits_after_reading  = renderer.deviceWaitCount();

    bool        ok            = true;
    const auto* light_sample  = sampleForTarget(measured, "profile-light");
    const auto* heavy_sample  = sampleForTarget(measured, "profile-heavy");
    const auto* window_sample = sampleForTarget(measured, "window");
    if (!measured.enabled) {
        std::fprintf(stderr, "[selftest] FAIL: VINE_VSG_PROFILE was set but the session reports no profiler\n");
        ok = false;
    }
    if (!measured.timestamps_available) {
        // Not a defect of this backend: a device without timestamps has nothing to measure, and saying so is
        // the honest outcome (the switch is still exercised end to end above).
        std::fprintf(stderr, "[selftest] gpu profile: the device reports no timestamps — measurement skipped\n");
        renderer.shutdown();
        setEnvironmentFlag("VINE_VSG_PROFILE", false);
        return ok;
    }
    if (light_sample == nullptr || light_sample->gpu_ms <= 0.0) {
        std::fprintf(stderr, "[selftest] FAIL: no GPU time was measured for the light pass (%zu sample(s))\n",
                     measured.passes.size());
        ok = false;
    }
    if (heavy_sample == nullptr) {
        std::fprintf(stderr, "[selftest] FAIL: the heavy pass was not measured\n");
        ok = false;
    }
    if (window_sample == nullptr) {
        std::fprintf(stderr, "[selftest] FAIL: the window render graph was not measured\n");
        ok = false;
    }
    if (measured.age_frames < 1u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the profile is not behind the session — reading it must not wait for the device\n");
        ok = false;
    }
    if (waits_after_reading != waits_before_reading) {
        std::fprintf(stderr, "[selftest] FAIL: reading the profile stopped the device (%zu -> %zu waits)\n",
                     waits_before_reading, waits_after_reading);
        ok = false;
    }
    // The claim that makes this a measurement rather than a print: the number follows the WORK. Same frame,
    // same target size, same everything except the draw count -- so no stub and no constant can pass.
    if (heavy_sample != nullptr && light_sample != nullptr && heavy_sample->gpu_ms <= light_sample->gpu_ms) {
        std::fprintf(stderr,
                     "[selftest] FAIL: %d draws (%.3f ms) did not cost more GPU time than 1 draw (%.3f ms)\n",
                     kHeavyDraws, heavy_sample->gpu_ms, light_sample->gpu_ms);
        ok = false;
    }
    if (ok) {
        // The NUMBERS are a trace, not an evidence line: they are measured milliseconds, and the evidence
        // baseline has to stay byte-comparable (the one drifting counter it already carries is a known debt,
        // see .ai/memory/graphics-perf-backlog.md).
        std::fprintf(stderr,
                     "[gpu-profile] frame %.3f ms; light 1 draw = %.3f ms; heavy %d draws = %.3f ms (x%.1f); "
                     "window = %.3f ms; age %llu frame(s)\n",
                     measured.frame_gpu_ms, light_sample->gpu_ms, kHeavyDraws, heavy_sample->gpu_ms,
                     heavy_sample->gpu_ms / light_sample->gpu_ms, window_sample->gpu_ms,
                     static_cast<unsigned long long>(measured.age_frames));
    }

    // Attribution and freshness. Three claims at once, and each of them excludes a different way of being
    // wrong:
    //   * the released target must be gone (a profile that reported whatever the log still held fails here);
    //   * the target that keeps rendering must be there (so "gone" cannot be the whole profile);
    //   * a target created AFTER the measurements must be there too. That last one is what separates
    //     "the newest frame that has results" from "any frame the log still holds": an older frame predates
    //     the new target by construction, so a reader that walked the log the other way round would lose it.
    renderer.releaseRenderTarget(heavy.target.get());
    ProfilePass late = makeProfilePass(u8"profile-late");
    for (int frame = 0; frame < kProfileFrames; ++frame) {
        FrameScope scope(renderer);
        driveProfilePassFrame(renderer, light, light_commands, 0, camera);
        driveProfilePassFrame(renderer, late, light_commands, 1, camera);
    }
    const auto after_release = renderer.gpuProfile();
    if (after_release.passes.empty() || sampleForTarget(after_release, "profile-heavy") != nullptr ||
        sampleForTarget(after_release, "profile-light") == nullptr ||
        sampleForTarget(after_release, "profile-late") == nullptr) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the profile does not describe the newest measured frame: %zu sample(s), "
                     "released=%d live=%d added-later=%d\n",
                     after_release.passes.size(), sampleForTarget(after_release, "profile-heavy") != nullptr,
                     sampleForTarget(after_release, "profile-light") != nullptr,
                     sampleForTarget(after_release, "profile-late") != nullptr);
        ok = false;
    }
    else {
        std::fprintf(stderr, "[gpu-profile] after the release: %zu sample(s), the live and the new one among them\n",
                     after_release.passes.size());
    }

    renderer.shutdown();
    setEnvironmentFlag("VINE_VSG_PROFILE", false);

    // The control: the same switch, off, driving the same kind of content. Nothing is measured, which is what
    // makes every number above the switch's doing rather than the session's.
    if (!renderer.initialize()) {
        std::fprintf(stderr, "[selftest] FAIL: the control session (no VINE_VSG_PROFILE) could not come up\n");
        return false;
    }
    // The LIGHT workload on purpose: what is under test here is the switch, and the heavy one would only make
    // the phase slower.
    ProfilePass control_pass = makeProfilePass(u8"profile-control");
    for (int frame = 0; frame < kProfileFrames; ++frame) {
        FrameScope scope(renderer);
        driveProfilePassFrame(renderer, control_pass, light_commands, 0, camera);
    }
    const auto control = renderer.gpuProfile();
    if (control.enabled || !control.passes.empty()) {
        std::fprintf(stderr, "[selftest] FAIL: a session without VINE_VSG_PROFILE reported a GPU profile\n");
        ok = false;
    }
    else if (ok) {
        // One stable line for the whole phase: the four claims together, so the baseline records what was
        // proven without recording numbers that change between runs.
        std::fprintf(stderr,
                     "[selftest] gpu profile: %zu pass(es) measured in one frame, the heavier workload cost "
                     "more, a released target left the profile, reading it cost no device wait, and without "
                     "VINE_VSG_PROFILE the session measures nothing\n",
                     measured.passes.size());
    }
    return ok;
}

} // namespace selftest
