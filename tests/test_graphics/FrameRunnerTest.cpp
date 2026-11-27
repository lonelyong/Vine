#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/TriangleMesh.hpp>

#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/View.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/Scene.hpp>

#include "FrameExpand.hpp"
#include "FramePump.hpp"
#include "FrameRunner.hpp"
#include "PublicationStore.hpp"
#include "SnapshotSource.hpp"

using vn::intrusive_ptr;
using vn::graphics::CameraSnapshot;
using vn::graphics::DrawItem;
using vn::graphics::DrawKind;
using vn::graphics::DrawPacket;
using vn::graphics::FrameDescription;
using vn::graphics::GeometryPtr;
using vn::graphics::Group;
using vn::graphics::MatrixTransform;
using vn::graphics::PassDraw;
using vn::graphics::Scene;
using vn::graphics::ScenePublishStatus;
using vn::graphics::View;
using vn::graphics::detail::expandFrame;
using vn::graphics::detail::FramePump;
using vn::graphics::detail::FrameRunner;
using vn::graphics::detail::PublicationStore;
using vn::graphics::detail::RunnerMode;
using vn::graphics::detail::SnapshotSource;
using vn::math::Vec3d;
using vn::math::Vec3f;

namespace
{

/// @brief A frame value whose one pass draws one view of @p content (the shape RenderPass::describe produces).
FrameDescription onePassFrame(const vn::graphics::DrawListSource& content){
    CameraSnapshot camera;
    camera.present    = true;
    camera.eye        = Vec3d(0.0, 0.0, 10.0);
    camera.target     = Vec3d(0.0, 0.0, 0.0);
    camera.up         = Vec3d(0.0, 1.0, 0.0);
    camera.view       = vn::math::Mat4d{};
    camera.projection = vn::math::Mat4d{};

    DrawPacket draw;
    draw.kind       = DrawKind::Scene;
    draw.view.camera = camera;
    draw.content    = &content;
    draw.view.layer_mask = vn::graphics::kAllLayers;

    PassDraw pass;
    pass.packets.push_back(draw);

    FrameDescription frame;
    frame.passes.emplace_back();
    frame.passes.back().draw = std::move(pass);
    return frame;
}

/// @brief Two triangles, one of them under a transform: enough that a wrong fold shows up in a matrix.
intrusive_ptr<Group> twoLeafAssembly()
{
    auto mesh = intrusive_ptr<vn::models::TriangleMesh>(new vn::models::TriangleMesh());
    mesh->setPositions({ Vec3f(0.0F, 0.0F, 0.0F), Vec3f(0.5F, 0.0F, 0.0F), Vec3f(0.0F, 0.5F, 0.0F) });
    mesh->bumpRevision();

    auto root = intrusive_ptr<Group>(new Group());
    auto arm  = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    arm->setMatrix(vn::math::translate(0.2, 0.1, 0.0));
    arm->addChild(vn::graphics::geometryFromShape(*mesh));
    root->addChild(arm);
    auto other = intrusive_ptr<vn::models::TriangleMesh>(new vn::models::TriangleMesh());
    other->setPositions({ Vec3f(-0.6F, 0.0F, 0.0F), Vec3f(-0.1F, 0.0F, 0.0F), Vec3f(-0.6F, 0.5F, 0.0F) });
    other->bumpRevision();
    root->addChild(vn::graphics::geometryFromShape(*other));
    return root;
}

/// @brief Collects the commands of the frame's one draw.
const std::vector<DrawItem>& commandsOf(const FrameDescription& frame)
{
    static const std::vector<DrawItem> kNone;
    if (frame.passes.empty() || !frame.passes.front().draw.has_value() || frame.passes.front().draw->packets.empty()) {
        return kNone;
    }
    const auto& commands = frame.passes.front().draw->packets.front().commands;
    return commands != nullptr ? *commands : kNone;
}

/// @brief Asserts two command lists are the same picture, field for field.
void expectSamePicture(const std::vector<DrawItem>& actual, const std::vector<DrawItem>& expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
        EXPECT_EQ(actual[index].geometry.get(), expected[index].geometry.get()) << "command " << index;
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                EXPECT_DOUBLE_EQ(actual[index].model_matrix(row, column), expected[index].model_matrix(row, column))
                    << "command " << index << ": element (" << row << ", " << column << ")";
            }
        }
    }
}

} // namespace

TEST(FrameRunnerTest, BothModesExpandAFrameToTheSamePicture)
{
    // THE TWO MODES ARE ONE CODE PATH. The inline runner is the caller's thread and the threaded one hands the frame
    // value over, waits for that frame, and gives it back expanded: the answer a frame gets must not depend on which.
    // The content's own answer is the third witness - it is what a frame named before any publication existed.
    auto root = twoLeafAssembly();
    Scene scene;
    scene.setRoot(root);

    // The content's own answer, for the SAME view the frames state (a query on the thread that owns it).
    View view;
    view.camera.eye = Vec3d(0.0, 0.0, 10.0);
    const std::vector<DrawItem> from_content = *scene.collectDrawItems(view);

    // A JOB IS ONE FRAME'S ENGINE HALF (see RenderEngine::runEngineHalf): the expansion of a value that already names
    // the publications it draws, which is what a caller without a device has to hand over.
    PublicationStore inline_store;
    ASSERT_TRUE(scene.publish());
    SnapshotSource* const inline_source = inline_store.sourceFor(scene, inline_store.publicationFor(scene));
    ASSERT_NE(inline_source, nullptr);
    FrameRunner     inline_runner{ RunnerMode::Inline };
    FrameDescription from_inline = onePassFrame(*inline_source);
    inline_runner.run([&] { expandFrame(from_inline); });
    EXPECT_EQ(inline_runner.frames(), 1U) << "jobs are counted where they run: inline, that is the caller's thread";

    PublicationStore threaded_store;
    SnapshotSource* const threaded_source = threaded_store.sourceFor(scene, threaded_store.publicationFor(scene));
    ASSERT_NE(threaded_source, nullptr);
    FrameRunner     threaded_runner{ RunnerMode::Threaded };
    FrameDescription from_thread = onePassFrame(*threaded_source);
    threaded_runner.run([&] { expandFrame(from_thread); });
    EXPECT_EQ(threaded_runner.frames(), 1U) << "the engine thread ran that frame";

    ASSERT_EQ(from_content.size(), 2U);
    expectSamePicture(commandsOf(from_inline), from_content);
    expectSamePicture(commandsOf(from_thread), from_content);
}

TEST(FrameRunnerTest, ManyFramesThroughTheEngineThreadStayEquivalent)
{
    // A thread that survives frames is the point: the request/completion handshake has to work over and over, with
    // nothing published in between (so every frame after the first expands from the same snapshot).
    auto root = twoLeafAssembly();
    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    ASSERT_TRUE(scene.publish());
    SnapshotSource* const source = store.sourceFor(scene, store.publicationFor(scene));
    ASSERT_NE(source, nullptr);
    FrameRunner runner{ RunnerMode::Threaded };

    View view;
    view.camera.eye = Vec3d(0.0, 0.0, 10.0);

    constexpr int kFrames = 64;
    for (int frame_index = 0; frame_index < kFrames; ++frame_index) {
        FrameDescription frame = onePassFrame(*source);
        runner.run([&] { expandFrame(frame); });
        ASSERT_EQ(commandsOf(frame).size(), 2U) << "frame " << frame_index;
        const std::vector<DrawItem> reference = *scene.collectDrawItems(view);
        expectSamePicture(commandsOf(frame), reference);
    }
    EXPECT_EQ(runner.frames(), static_cast<std::uint64_t>(kFrames));
    EXPECT_EQ(store.publicationCount(), 1U);
}

TEST(FrameRunnerTest, StoppedRunnerRunsFramesOnTheCallerInsteadOfWaiting)
{
    // stop() is idempotent, and what is left is an INLINE runner - not a runner that waits for a thread that is gone.
    // That is the property a caller without a mode switch depends on: asking for a frame never deadlocks.
    auto root = twoLeafAssembly();
    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    ASSERT_TRUE(scene.publish());
    SnapshotSource* const source = store.sourceFor(scene, store.publicationFor(scene));
    ASSERT_NE(source, nullptr);
    FrameRunner runner{ RunnerMode::Threaded };
    runner.stop();
    runner.stop(); // idempotent
    EXPECT_EQ(runner.frames(), 0U);

    FrameDescription frame = onePassFrame(*source);
    // The job is the engine half as it always is: a stopped runner runs it on the caller instead of waiting for a
    // thread that is gone - it does not reinterpret how the draw should be answered.
    runner.run([&] { expandFrame(frame); });
    EXPECT_EQ(commandsOf(frame).size(), 2U);
    EXPECT_EQ(frame.passes.front().draw->packets.front().content, source)
        << "the draw names the content's publication, not the content - which is what makes the frame portable";
    EXPECT_EQ(runner.busy(), false) << "nothing is left in flight";
    EXPECT_EQ(runner.mode(), RunnerMode::Threaded) << "the mode is a lifetime decision, not a live state";
}

TEST(FrameRunnerTest, AJobThatThrowsDoesNotTakeTheRunnerDownWithIt)
{
    // REVIEW 6 / P1-4. An exception leaving a thread function calls `std::terminate` - one bad job would end the
    // process - and one leaving `has_request_` set would block every later submit forever. The one-in-flight
    // handshake is the RUNNER's promise, so it holds whatever the job did; who the failure belongs to is the job's
    // business (RenderEngine::answerEngineHalf turns it into a frame answer).
    FrameRunner runner{ RunnerMode::Threaded };
    runner.run([] { throw std::runtime_error("synthetic job failure"); });
    EXPECT_EQ(runner.failures(), 1U) << "a job that threw still ran, and the runner says so";
    EXPECT_EQ(runner.frames(), 1U) << "it counts as a frame: the handshake completed";
    EXPECT_FALSE(runner.busy()) << "and nothing is left in flight";

    // The runner is still usable: the throw neither killed the thread nor left the handshake stuck.
    int ran = 0;
    runner.run([&ran] { ++ran; });
    EXPECT_EQ(ran, 1) << "the next job runs to completion";
    EXPECT_EQ(runner.frames(), 2U);
    EXPECT_EQ(runner.failures(), 1U);

    // INLINE IS THE CALLER'S THREAD, so the caller keeps the exception: a runner with no thread must not swallow a
    // failure the code that ran it is standing right next to.
    FrameRunner inline_runner{ RunnerMode::Inline };
    EXPECT_THROW(inline_runner.run([] { throw std::runtime_error("synthetic inline failure"); }), std::runtime_error);
    EXPECT_EQ(inline_runner.failures(), 1U) << "the count is kept here too";
}

/*
 * @brief The interval a session's shape is fixed in covers BOTH halves - the content one included.
 *
 * The gate used to ask the runner, which is false for every inline frame and false through the whole content half of a
 * threaded one: it was open exactly while the compile walked the table a shape change edits. The phase is the pump's
 * fact now, and it opens when a build starts and closes when the frame is ANSWERED - not when the job was submitted.
 */
TEST(FramePumpTest, ThePhaseCoversTheBuildAndTheAnswer)
{
    FramePump pump;
    EXPECT_FALSE(pump.recording()) << "a fresh pump is idle";
    {
        const FramePump::Build build = pump.beginFrame();
        EXPECT_EQ(build.frame().number, 1U) << "the first frame is numbered from one";
        EXPECT_TRUE(pump.recording()) << "the content half is the first half of the interval";
    }
    EXPECT_FALSE(pump.recording()) << "a build nobody answered ends the interval with its guard";
}

TEST(FramePumpTest, ThePhaseStaysOpenUntilAFrameHandedToItsThreadIsAnswered)
{
    FramePump pump;
    ASSERT_EQ(pump.setThreaded(true), FramePump::Build::Refusal::None);

    std::mutex              gate;
    std::condition_variable changed;
    bool                    entered  = false;
    bool                    released = false;

    {
        const FramePump::Build build = pump.beginFrame();
        pump.submit([&] {
            std::unique_lock<std::mutex> lock(gate);
            entered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return released; });
            // THE ANSWER CLOSES THE PHASE (see publishAnswer): one verb, so a frame cannot be half answered.
            pump.publishAnswer(1U, vn::graphics::PresentOutcome::Skipped, vn::graphics::FrameResult{}, {});
        });
        EXPECT_TRUE(pump.recording()) << "the frame was handed over, and the shape must stay put";
    }
    {
        std::unique_lock<std::mutex> lock(gate);
        changed.wait(lock, [&] { return entered; });
    }
    EXPECT_TRUE(pump.recording()) << "the engine half is the second half of the interval";
    EXPECT_FALSE(pump.wait(1U, std::chrono::milliseconds(0))) << "the frame is in flight, not answered";
    {
        const std::lock_guard<std::mutex> lock(gate);
        released = true;
    }
    changed.notify_all();
    EXPECT_TRUE(pump.wait(1U, std::chrono::milliseconds(5000)));
    EXPECT_FALSE(pump.recording()) << "an answered frame reopens the shape";
    pump.stop();
}

/*
 * @brief A frame the pump never handed out is answered "no" at once, not waited for.
 *
 * `waitForFrame` is what a UI thread blocks on (a screenshot, a settled resize), so a number nothing will ever complete
 * must not cost it the timeout - and must not make it wait for an unrelated frame that happens to be in flight.
 */
TEST(FramePumpTest, AFrameThePumpNeverHandedOutIsAnsweredWithoutWaitingForTheOneInFlight)
{
    FramePump pump;
    ASSERT_EQ(pump.setThreaded(true), FramePump::Build::Refusal::None);

    std::mutex              gate;
    std::condition_variable changed;
    bool                    entered  = false;
    bool                    released = false;

    {
        const FramePump::Build build = pump.beginFrame();
        pump.submit([&] {
            std::unique_lock<std::mutex> lock(gate);
            entered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return released; });
            pump.publishAnswer(1U, vn::graphics::PresentOutcome::Skipped, vn::graphics::FrameResult{}, {});
        });
    }
    {
        std::unique_lock<std::mutex> lock(gate);
        changed.wait(lock, [&] { return entered; });
    }

    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(pump.wait(2U, std::chrono::milliseconds(5000))) << "frame 2 was never handed out, so it is not pending";
    const auto spent = std::chrono::steady_clock::now() - started;
    EXPECT_LT(spent, std::chrono::milliseconds(1000)) << "the answer waited for frame 1's engine half instead";

    {
        const std::lock_guard<std::mutex> lock(gate);
        released = true;
    }
    changed.notify_all();
    EXPECT_TRUE(pump.wait(1U, std::chrono::milliseconds(5000))) << "and the frame that does exist still answers";
    pump.stop();
}

/*
 * @brief Two ways in, two answers: the interval names WHY an entry was refused, and a refused entry changes nothing.
 *
 * The pair is the whole point of `Build::Refusal` (design `graphics-threading.md` 2.17): "this thread already holds it"
 * is a call whose next step would wait for itself, "another thread holds it" is a second driver - two mistakes a host
 * fixes differently - and neither of them may take a frame number, rotate a slot or start a clock.
 */
TEST(FramePumpTest, TheIntervalAnswersWhyASecondEntryWasRefused)
{
    FramePump pump;

    // THIS THREAD, INSIDE THE INTERVAL: a frame asked while this thread builds one.
    {
        const FramePump::Build held = pump.beginFrame();
        ASSERT_TRUE(held);
        EXPECT_EQ(held.frame().number, 1U);

        const FramePump::Build again = pump.beginFrame();
        EXPECT_FALSE(again) << "a second frame is not queued behind the one this thread is inside";
        EXPECT_EQ(again.refusal(), FramePump::Build::Refusal::Reentrant) << "and the answer names the cause";

        const FramePump::Build prepared = pump.beginBuild();
        EXPECT_FALSE(prepared) << "a prepare is a build: the same interval, the same answer";
        EXPECT_EQ(prepared.refusal(), FramePump::Build::Refusal::Reentrant);
        EXPECT_EQ(pump.lastNumber(), 1U) << "a refused entry takes no frame number";
    }
    EXPECT_FALSE(pump.recording()) << "the owned build's guard is the one place the phase ends";

    // ANOTHER HOST THREAD, WHILE THIS ONE HOLDS IT: the second driver.
    std::mutex              gate;
    std::condition_variable changed;
    bool                    inside = false;
    bool                    leave  = false;
    std::thread             holder([&] {
        const FramePump::Build       build = pump.beginBuild();
        std::unique_lock<std::mutex> lock(gate);
        inside = true;
        changed.notify_all();
        changed.wait(lock, [&] { return leave; });
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(5), [&] { return inside; })) << "the holder is inside";
    }
    EXPECT_TRUE(pump.recording()) << "the interval is held from another thread";
    EXPECT_EQ(pump.beginFrame().refusal(), FramePump::Build::Refusal::Concurrent);
    EXPECT_EQ(pump.beginBuild().refusal(), FramePump::Build::Refusal::Concurrent);
    EXPECT_EQ(pump.lastNumber(), 1U) << "the refused callers took nothing";
    {
        const std::lock_guard<std::mutex> lock(gate);
        leave = true;
    }
    changed.notify_all();
    holder.join();

    // AND THE NEXT ENTRY IS ACCEPTED: a refusal is about the moment, not about the caller.
    EXPECT_FALSE(pump.recording());
    const FramePump::Build afterwards = pump.beginFrame();
    ASSERT_TRUE(afterwards);
    EXPECT_EQ(afterwards.frame().number, 2U) << "numbers continue where the accepted frame left them";
}

/*
 * @brief A build that leaves by an exception leaves the interval FREE - for this thread and for every other.
 *
 * The guard exists for exactly this path (see `Build`): a pass' `describe` that throws walks out of the content half,
 * and a phase left set would refuse every later frame for the life of the engine. The second half of the case asks from
 * ANOTHER thread, because the thread-local bookkeeping could hide the same leak from this one's point of view.
 */
TEST(FramePumpTest, ABuildThatLeavesByAnExceptionLeavesTheIntervalFree)
{
    FramePump pump;
    EXPECT_ANY_THROW({
        const FramePump::Build build = pump.beginFrame();
        EXPECT_TRUE(pump.recording());
        throw std::runtime_error("a pass' describe threw");
    });
    EXPECT_FALSE(pump.recording()) << "the guard ended the phase on the way out";

    FramePump::Build::Refusal refusal = FramePump::Build::Refusal::Concurrent;
    std::thread               other([&] { refusal = pump.beginBuild().refusal(); });
    other.join();
    EXPECT_EQ(refusal, FramePump::Build::Refusal::None) << "another thread finds the interval free, not half held";
    EXPECT_EQ(pump.lastNumber(), 1U) << "the abandoned frame still counted: it WAS handed out";
}

/*
 * @brief Two host threads drive in TURN, and every frame is numbered exactly once.
 *
 * "Two threads may still drive the engine in TURN - only not at once" (design 2.17) is a claim about the hand-off: each
 * entry claims, rotates and releases, so the next thread's write of `slot_` / `current_` is ordered after the previous
 * one's read. TSan is the witness; the numbers are the observable half - one per frame, no gap and no duplicate, which a
 * race would produce as two frames with one number.
 */
TEST(FramePumpTest, TwoHostThreadsDriveThePumpInTurnAndNumberEveryFrameOnce)
{
    FramePump                              pump;
    constexpr int                          kFrames = 40;
    std::vector<std::uint64_t>             numbers;
    std::vector<FramePump::Build::Refusal> refusals(static_cast<std::size_t>(kFrames), FramePump::Build::Refusal::Concurrent);
    numbers.reserve(static_cast<std::size_t>(kFrames));

    for (int frame = 0; frame < kFrames; ++frame) {
        // ONE ENTRY PER THREAD, joined before the next: the turns are the point, and a thread per turn keeps them honest
        // (a loop in one thread would never test the hand-off at all).
        std::thread driver([&pump, &numbers, &refusals, frame] {
            const FramePump::Build build              = pump.beginFrame();
            refusals[static_cast<std::size_t>(frame)] = build.refusal();
            if (build) {
                numbers.push_back(build.frame().number);
            }
        });
        driver.join();
    }

    for (const FramePump::Build::Refusal refusal : refusals) {
        EXPECT_EQ(refusal, FramePump::Build::Refusal::None) << "a turn taken after the previous one ended is not a queue";
    }
    ASSERT_EQ(numbers.size(), static_cast<std::size_t>(kFrames));
    for (int frame = 0; frame < kFrames; ++frame) {
        EXPECT_EQ(numbers[static_cast<std::size_t>(frame)], static_cast<std::uint64_t>(frame) + 1U) << "frame " << frame << " got its own number";
    }
    EXPECT_EQ(pump.lastNumber(), static_cast<std::uint64_t>(kFrames));
}

/*
 * @brief One thread may hold two pumps at once: two engines are two intervals, and each build puts back what it found.
 *
 * This is the invariant `Build::previous_` exists for. A single thread-local would read "this thread is building" for
 * BOTH engines, so a sink driving a second engine from inside the first one's frame would be refused as a re-entry of
 * an interval it does not hold - and the outer build's ownership would be lost on the way back.
 */
TEST(FramePumpTest, OneThreadMayHoldTwoPumpsAndEachKeepsItsOwnInterval)
{
    FramePump first;
    FramePump second;
    {
        const FramePump::Build outer = first.beginBuild();
        ASSERT_TRUE(outer) << "the first engine's interval is free";
        {
            const FramePump::Build inner = second.beginBuild();
            EXPECT_TRUE(inner) << "another engine's interval is not the first one's";
        }
        EXPECT_FALSE(second.recording());
        const FramePump::Build again = first.beginBuild();
        EXPECT_FALSE(again) << "the inner build put back the entry it displaced";
        EXPECT_EQ(again.refusal(), FramePump::Build::Refusal::Reentrant);
    }
    EXPECT_FALSE(first.recording());
    EXPECT_FALSE(second.recording());
    EXPECT_TRUE(first.beginBuild()) << "and both are free again once their own builds ended";
    EXPECT_TRUE(second.beginBuild());
}

/*
 * @brief A thread-mode change is an INTERVAL VERB: the same two refusals a frame gets, and never a mode half applied.
 *
 * It changes the fact a frame asks before it builds ("where does my other half run") and switching back joins the thread
 * an answer runs on, so it takes the interval instead of writing `runner_` beside the frame's reads of it (design
 * `graphics-threading.md` 2.17).
 */
TEST(FramePumpTest, AThreadModeChangeIsAnIntervalVerb)
{
    FramePump pump;
    {
        const FramePump::Build build = pump.beginFrame();
        ASSERT_TRUE(build);
        EXPECT_EQ(pump.setThreaded(true), FramePump::Build::Refusal::Reentrant) << "the mode a build is running under cannot change from inside it";
        EXPECT_FALSE(pump.threaded()) << "and the mode did not change";
    }

    std::mutex              gate;
    std::condition_variable changed;
    bool                    inside = false;
    bool                    leave  = false;
    std::thread             holder([&] {
        const FramePump::Build       build = pump.beginBuild();
        std::unique_lock<std::mutex> lock(gate);
        inside = true;
        changed.notify_all();
        changed.wait(lock, [&] { return leave; });
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(5), [&] { return inside; }));
    }
    EXPECT_EQ(pump.setThreaded(true), FramePump::Build::Refusal::Concurrent)
        << "a frame being built on another host thread is not a mode change's to interrupt";
    EXPECT_FALSE(pump.threaded());
    {
        const std::lock_guard<std::mutex> lock(gate);
        leave = true;
    }
    changed.notify_all();
    holder.join();

    // BETWEEN FRAMES IT LANDS, and `threaded()` - the fact `frame()` reads - follows it both ways.
    EXPECT_EQ(pump.setThreaded(true), FramePump::Build::Refusal::None);
    EXPECT_TRUE(pump.threaded());
    EXPECT_EQ(pump.setThreaded(false), FramePump::Build::Refusal::None);
    EXPECT_FALSE(pump.threaded()) << "the thread was joined, so the pump is inline again";
}

/*
 * @brief A stop asked from the engine thread leaves an INLINE pump - before the join, not after it (review 19 Z21).
 *
 * `threaded()` is the fact `frame()` reads to decide where its other half runs. It used to be "a runner object is
 * stored", and a stop from the engine thread cannot join itself: the object stayed, its loop had left, and the next
 * frame was handed to it and never ran. The answer now comes from the runner's own state (`FrameRunner::accepting`),
 * so it is false from the moment the stop is asked for.
 */
TEST(FramePumpTest, AStopAskedFromTheFrameThreadLeavesAnInlinePump)
{
    FramePump pump;
    ASSERT_EQ(pump.setThreaded(true), FramePump::Build::Refusal::None);
    ASSERT_TRUE(pump.threaded());

    std::atomic<bool> threaded_inside{ true };
    std::promise<void> stopped;
    std::future<void>  stopped_future = stopped.get_future();
    pump.submit([&]() {
        pump.stop(); // the frame asking for its own teardown
        threaded_inside = pump.threaded();
        stopped.set_value();
    });
    ASSERT_EQ(stopped_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    pump.waitIdle();
    EXPECT_FALSE(threaded_inside.load()) << "the answer changes with the stop, not with the join";
    EXPECT_FALSE(pump.threaded());

    // AND THE NEXT FRAME RUNS HERE instead of vanishing into the loop that left: the runner runs a job it cannot hand
    // over on the caller's thread, which is what `frame()` now reads from `threaded()`.
    std::atomic<bool> ran{ false };
    pump.submit([&ran]() { ran = true; });
    EXPECT_TRUE(ran.load());
}
