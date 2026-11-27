#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>

#include "FrameRunner.hpp"

using vn::graphics::detail::FrameRunner;
using vn::graphics::detail::RunnerMode;

TEST(FrameRunnerReentryTest, AJobThatWaitsForItselfAnswersInsteadOfHanging)
{
    // A FRAME CANNOT WAIT FOR ITSELF (design graphics-threading.md 2.12). `waitIdle` / `wait` exist for the thread that
    // is NOT the engine thread; asked from the job itself they used to block on `has_request_`, which only that job can
    // clear - a hang with no timeout and no diagnostic. A hook or a diagnostic sink driving the same engine reaches
    // exactly this state, so the answer is "not answered yet", at once.
    FrameRunner runner{ RunnerMode::Threaded };

    std::atomic<bool> asked_inside{ false };
    std::atomic<bool> waited_inside{ true };
    std::atomic<bool> ran_inside{ false };
    runner.submit([&]() {
        runner.waitIdle(); // without the guard this never returns: the job waits for its own completion
        waited_inside = runner.wait(std::chrono::milliseconds(0));
        asked_inside  = true;
        ran_inside    = true;
    });

    // The test thread is NOT the engine thread, so its own wait is the ordinary one - and it completes only because the
    // job answered instead of waiting for itself.
    EXPECT_TRUE(runner.wait(std::chrono::milliseconds(5000))) << "the job finished; a self-wait would have hung here";
    EXPECT_TRUE(asked_inside.load()) << "the job reached the question";
    EXPECT_TRUE(ran_inside.load()) << "and ran to its end";
    EXPECT_FALSE(waited_inside.load()) << "the frame the job is inside is not 'answered' - the honest answer is false";

    runner.stop();
    EXPECT_FALSE(runner.busy());
}

TEST(FrameRunnerReentryTest, AStopAskedFromTheFrameThreadLeavesTheJoinToAnotherThread)
{
    // A THREAD CANNOT JOIN ITSELF. `stop()` from the job asks the loop to leave (stopping_ is set, so it exits after the
    // job) and returns; the join happens in the next stop() from another thread - which is what the pump's destructor
    // and shutdown() do. Without this the call from the job is a self-join: `std::system_error` or a terminate.
    FrameRunner runner{ RunnerMode::Threaded };

    std::promise<void> asked;
    std::future<void>  asked_future = asked.get_future();
    runner.submit([&runner, &asked]() {
        runner.stop(); // asked from the frame thread: returns, does not join itself
        asked.set_value();
    });
    ASSERT_EQ(asked_future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "the job's own stop() returned instead of joining its thread";
    runner.stop(); // the join, from a thread that is not the frame thread
    EXPECT_FALSE(runner.busy());
    runner.stop(); // idempotent
}

TEST(FrameRunnerReentryTest, ARunnerAStopHasLeftRunsTheNextJobOnTheCaller)
{
    // REVIEW 19 Z21. A stop asked from the frame thread cannot join itself, so the loop leaves after its last job - but
    // the runner used to keep advertising a live thread (`live_` was cleared only by the join from another thread).
    // `submit` then took the threaded branch and handed the job to a loop that had exited: accepted, never run, and
    // `has_request_` stayed set, so every later submit (and `waitIdle`) blocked for ever. "Where does my job run" is ONE
    // fact now (`accepting()`), and it answers "here" from the moment the stop is asked for.
    FrameRunner runner{ RunnerMode::Threaded };

    std::atomic<bool> accepting_inside{ true };
    std::atomic<bool> frame_thread_inside{ false };
    std::promise<void> stopped;
    std::future<void>  stopped_future = stopped.get_future();
    runner.submit([&]() {
        runner.stop(); // asked from the frame thread: returns, the loop leaves after this job
        accepting_inside     = runner.accepting();
        frame_thread_inside  = runner.onRunnerThread();
        stopped.set_value();
    });
    ASSERT_EQ(stopped_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(runner.wait(std::chrono::milliseconds(5000))) << "the job in flight finished";
    EXPECT_TRUE(frame_thread_inside.load()) << "the thread is still the frame thread while it finishes its last job";
    EXPECT_FALSE(accepting_inside.load()) << "the loop takes no new job from the moment the stop is asked for";

    // THE ZOMBIE ITSELF: the job submitted now must RUN (on this thread), not be handed to a loop that has left.
    std::atomic<bool> ran{ false };
    runner.submit([&ran]() { ran = true; });
    EXPECT_TRUE(ran.load()) << "a job a stop has left is run on the caller, not dropped";
    EXPECT_TRUE(runner.wait(std::chrono::milliseconds(0))) << "and nothing is left in flight";
    EXPECT_FALSE(runner.busy());
    EXPECT_EQ(runner.frames(), 2U);

    runner.stop(); // the join the frame thread could not do
    EXPECT_FALSE(runner.accepting());
    EXPECT_EQ(runner.mode(), RunnerMode::Threaded) << "the mode is a lifetime decision; only the thread is gone";
}
