#include "FramePump.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

FramePump::FramePump()
  : slots_{ std::make_unique<EngineFrame>(), std::make_unique<EngineFrame>() }
{
    // THE CURRENT VALUE EXISTS FROM THE START (see frameDescription): before the first frame it is the empty value a
    // host reads, not a null pointer.
    current_ = slots_[0].get();
}

FramePump::~FramePump()
{
    stop();
}

FramePump::Build::Build(FramePump& pump, EngineFrame& frame) noexcept
  : pump_(&pump)
  , frame_(&frame)
  , previous_(s_building_)
{
    // THE OWNERSHIP IS TAKEN BY claimInterval (it has to be, before the frame value may be handed out), so this
    // constructor only records WHO holds it: every later question - "is the holder me?" - is answered from this
    // thread-local, and the destructor puts back the entry it found.
    s_building_ = &pump;
}

FramePump::Build::~Build()
{
    // THE ONE PLACE THE BUILD'S PHASE ENDS, on every path (an early return, a throwing describe, the ordinary one). A
    // built frame stays open through `answering` (set by submit, cleared by publishAnswer); a prepare never answers, so
    // this store is the whole of its interval. A REFUSED build ends nothing: it never held the phase.
    if (pump_ != nullptr) {
        // THE THREAD-LOCAL IS THIS BUILD'S TO PUT BACK: it recorded the entry it found when it started (a build of
        // ANOTHER pump, or none), so a hook driving another engine leaves the outer entry intact.
        s_building_ = previous_;
        pump_->releaseInterval();
    }
}

void FramePump::releaseInterval() noexcept
{
    // ONLY THE PHASE IS CLEARED HERE: the thread-local belongs to the BUILD that took the interval, which is the object
    // that knows what it displaced (see ~Build) - `takeInterval`'s refused-after-win path on the engine thread owns no
    // build and must not touch it (it never displaced anything: an open build of this pump would have failed the claim).
    const std::lock_guard<std::mutex> gate(phase_mutex_);
    compiling_.store(false, std::memory_order_release);
}

bool FramePump::claimInterval() noexcept
{
    // ONE OWNER, AND NO WAITING TO BECOME ONE (see Build): a second entry is refused rather than queued, because a
    // queue of frames is not a pipeline this design has (2.12 / D12) and because a blocked caller here would be the
    // host waiting for the host's own hook. The gate is the same one a shape change holds, so "nothing is
    // recording" and "the phase flips" cannot be decided on either side of each other.
    const std::lock_guard<std::mutex> gate(phase_mutex_);
    bool                              free = false;
    return compiling_.compare_exchange_strong(free, true, std::memory_order_acq_rel, std::memory_order_acquire);
}

FramePump::Build::Refusal FramePump::takeInterval() noexcept
{
    // ONE ENTRY, TWO REASONS (see Build): a claim that loses is told apart by the thread-local - "this thread holds
    // it" is a call from inside a build whose next step would wait for itself, "another thread holds it" is a second
    // driver. A claim that WINS on the engine thread is the same self-wait one step later (`waitIdle` would wait for
    // the job this thread is inside), so it is refused here, before anything reads a slot - which is what lets
    // `frame()` / `prepare()` / `setThreaded()` state the rule once, in the class that owns the thread.
    if (!claimInterval()) {
        return s_building_ == this ? Build::Refusal::Reentrant : Build::Refusal::Concurrent;
    }
    if (onEngineThread()) {
        releaseInterval();
        return Build::Refusal::Reentrant;
    }
    return Build::Refusal::None;
}

EngineFrame& FramePump::Build::frame() const noexcept
{
    return *frame_;
}

FramePump::Build FramePump::beginFrame()
{
    // A SECOND FRAME IS REFUSED, NOT QUEUED (see takeInterval): the interval is claimed BEFORE anything is read or
    // moved, so a caller on another thread - and the engine's own thread asking about a frame it is inside - is
    // answered here, and the slot below is only rotated by the winner.
    const Build::Refusal refusal = takeInterval();
    if (refusal != Build::Refusal::None) {
        return Build{ refusal };
    }
    // ONE FRAME IN FLIGHT: the previous engine half is finished before this value moves, so a caller cannot queue
    // frames and no two jobs ever touch the publications or the device at once (design 2.12, D12). The wait happens
    // BEFORE the value is handed out, so a shape change that lands during it is answered Busy at once (the frame is
    // still ANSWERING); the interval itself is already this caller's, which is what makes a second thread's entry a
    // refusal rather than a queue behind this wait.
    waitIdle();
    EngineFrame& next = *slots_[slot_];
    slot_             = (slot_ + 1) % slots_.size();
    next.number       = number_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    current_          = &next;
    return Build{ *this, next };
}

FramePump::Build FramePump::beginBuild()
{
    // A PREPARE CLAIMS THE SAME INTERVAL (see takeInterval): it walks the same tables, so the rules - one owner,
    // refused not queued, and never for the thread that is already inside - are one rule, not three.
    const Build::Refusal refusal = takeInterval();
    if (refusal != Build::Refusal::None) {
        return Build{ refusal };
    }
    return Build{ *this, *current_ };
}

EngineFrame& FramePump::currentFrame() noexcept
{
    return *current_;
}

const EngineFrame& FramePump::currentFrame() const noexcept
{
    return *current_;
}

void FramePump::submit(Job job)
{
    // THE ENGINE HALF OWNS THE REST OF THE FRAME (see recording): from here until publishAnswer the shape must stay
    // put, whichever thread runs it. Set before the hand-over so the caller that just returned from frame() sees the
    // frame as open, not merely as "a job the runner may not have picked up yet".
    answering_.store(true, std::memory_order_release);
    try {
        // THE RUNNER IS COPIED, NOT READ TWICE (see runner_): this caller holds the interval, so no mode change can be
        // in flight - and the copy is what a hand-over needs if one ever were.
        const std::shared_ptr<FrameRunner> runner = runner_.load(std::memory_order_acquire);
        if (runner == nullptr) {
            // Inline: the calling thread IS the engine thread, so "hand it over" has nowhere else to go. The answer is
            // complete when this returns - which is what `frame()` reports as Ran.
            job();
            return;
        }
        runner->submit(std::move(job));
    }
    catch (...) {
        // A FAILED HAND-OVER LEAVES NOTHING TO ANSWER: the frame was never built, so the phase must not stay closed
        // (a stuck `answering` would refuse every later shape change for the life of the engine).
        answering_.store(false, std::memory_order_release);
        throw;
    }
}

void FramePump::waitIdle()
{
    const std::shared_ptr<FrameRunner> runner = runner_.load(std::memory_order_acquire);
    if (runner != nullptr) {
        runner->waitIdle();
    }
}

bool FramePump::wait(std::uint64_t number, std::chrono::milliseconds timeout)
{
    if (number == 0U) {
        return false; // no frame was ever handed out, so nothing can be "answered"
    }
    // A NUMBER THIS PUMP NEVER HANDED OUT IS AN ANSWER, NOT A WAIT: nothing will ever complete it, and a host that
    // asked about a frame that does not exist (a stale id, a caller's own arithmetic) must not burn the timeout - and
    // must not hold up a UI thread for five seconds to learn it (`G4`: "not built" and "not answered yet" are
    // different facts, and an early "no" is the honest answer to the first).
    if (number > number_.load(std::memory_order_acquire)) {
        return false;
    }
    if (completed_.load(std::memory_order_acquire) >= number) {
        return true;
    }
    // THE WAIT IS ON THE RUNNER, NOT ON THIS PUMP'S IDEA OF ONE (`G4`): a host may wait from any thread, and a host
    // may also switch the mode from another one - so the object is taken by shared ownership and the wait cannot
    // outlive it. A runner that was stopped meanwhile answers "nothing is in flight", which is the truth: the frame
    // was answered, or it will never be.
    const std::shared_ptr<FrameRunner> runner = runner_.load(std::memory_order_acquire);
    if (runner == nullptr) {
        // Inline: every frame this pump handed out was answered before submit() returned, so a number it does not know
        // is simply not built yet.
        return completed_.load(std::memory_order_acquire) >= number;
    }
    (void)runner->wait(timeout);
    return completed_.load(std::memory_order_acquire) >= number;
}

std::uint64_t FramePump::lastNumber() const noexcept
{
    return number_.load(std::memory_order_acquire);
}

bool FramePump::threaded() const noexcept
{
    // A PUBLISHED FACT, not a lookup of a pointer another thread may be replacing (see runner_): this is what a HUD or
    // a benchmark asks while frames run.
    return runner_.load(std::memory_order_acquire) != nullptr;
}

bool FramePump::onEngineThread() const noexcept
{
    // THE CALLER ASKS ABOUT ITSELF, so it is asked of the runner that exists for it: taken by shared ownership, because
    // `RenderEngine::shutdown` is the one caller that asks while another thread may be driving (see stop).
    const std::shared_ptr<FrameRunner> runner = runner_.load(std::memory_order_acquire);
    return runner != nullptr && runner->onRunnerThread();
}

bool FramePump::recording() const
{
    // THE TWO HALVES OF THE INTERVAL (see the header): the content half this thread may be inside, and the engine half
    // somebody else may be running. Both are atomics, so this is answerable from any thread - which is what a shape
    // change from an application's own pool needs.
    return compiling_.load(std::memory_order_acquire) || answering_.load(std::memory_order_acquire);
}

std::unique_lock<std::mutex> FramePump::phaseGate()
{
    return std::unique_lock<std::mutex>(phase_mutex_);
}

FramePump::Build::Refusal FramePump::setThreaded(bool threaded)
{
    // THE MODE IS AN INTERVAL FACT (see the header): it is what a frame asks before it builds ("where does my other
    // half run"), and switching back JOINS the thread an answer runs on - so the switch enters the interval like any
    // other entry and answers the same two refusals. On the engine thread `takeInterval` refuses first, before anything
    // here could try to join the thread it is running on.
    const Build::Refusal refusal = takeInterval();
    if (refusal != Build::Refusal::None) {
        return refusal;
    }
    // THE INTERVAL ENDS WHEN THIS RETURNS, WHICHEVER WAY IT GOES: the same guard a build uses, over the value the last
    // compile filled (nothing is compiled here, so that value is never read).
    const Build lease{ *this, *current_ };
    if (threaded == (runner_.load(std::memory_order_acquire) != nullptr)) {
        return Build::Refusal::None; // already there
    }
    if (threaded) {
        // The thread exists only when a host asked for one: an engine that never does allocates nothing and starts
        // nothing, and the inline path stays literally the same code.
        runner_.store(std::make_shared<FrameRunner>(RunnerMode::Threaded), std::memory_order_release);
        return Build::Refusal::None;
    }
    // Back to inline: a frame in flight is finished first (the thread is joined, not detached), so nothing is being
    // answered while the host goes on to describe frames here. The store comes BEFORE the join, so a thread that is
    // waiting (see wait) already sees the inline answer; a waiter that took the old runner by shared ownership finishes
    // on a stopped object, which answers "nothing is in flight" - the truth, since the frame was answered.
    const std::shared_ptr<FrameRunner> runner = runner_.load(std::memory_order_acquire);
    runner->waitIdle();
    runner_.store(nullptr, std::memory_order_release);
    runner->stop();
    return Build::Refusal::None;
}

void FramePump::publishAnswer(std::uint64_t number, PresentOutcome outcome, FrameResult result, std::span<const PassCost> pass_costs)
{
    // THE PICTURE'S FATE AND THE FRAME'S ID TRAVEL AS ONE WORD (see present_): a host that reads the record while the
    // next frame is being answered must never see this frame's outcome beside that frame's id.
    present_.store((number << 8U) | static_cast<std::uint64_t>(outcome), std::memory_order_release);
    // THE WHOLE ANSWER LANDS TOGETHER: what the frame consisted of and what each of its passes cost are two halves of
    // one fact a host reads, so they share this lock. The cost vector is a copy because the frame value it came from is
    // reused by a later frame the moment this one is answered.
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        result_ = std::move(result);
        answered_pass_costs_.assign(pass_costs.begin(), pass_costs.end());
    }
    // ...AND THE FRAME IS ANSWERED LAST (what waitForFrame observes, and the release that publishes the two facts above
    // to a host that waited), together with the phase it closes: the engine half is over, so the shape may change again.
    answering_.store(false, std::memory_order_release);
    completed_.store(number, std::memory_order_release);
}

FrameResult FramePump::result() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

std::vector<PassCost> FramePump::answeredPassCosts() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return answered_pass_costs_;
}

std::uint64_t FramePump::packedPresent() const noexcept
{
    return present_.load(std::memory_order_acquire);
}

void FramePump::stop()
{
    // THE RUNNER IS TAKEN ONCE (see runner_): a waitForFrame from another thread may be holding it at the same moment,
    // and the answer to "is anything in flight" must not depend on which of the two got there first.
    const std::shared_ptr<FrameRunner> runner = runner_.load(std::memory_order_acquire);
    if (runner != nullptr) {
        runner->waitIdle();
        runner->stop(); // a call from the frame thread only asks it to leave (see FrameRunner::stop)
        if (onEngineThread()) {
            // THE JOIN BELONGS TO ANOTHER THREAD: the loop exits after the job it is inside, and the next stop() - from
            // the destructor or from shutdown() on the host's thread - performs it.
            return;
        }
        runner_.store(nullptr, std::memory_order_release);
    }
    // NOTHING IS ANSWERING ANY MORE: the phase this pump owns is left where the next frame may start from, whether the
    // pump was stopped between frames or from its own frame thread (the frame it was inside has been answered by the
    // time its job returned).
    answering_.store(false, std::memory_order_release);
}

} // namespace detail

VN_GRAPHICS_NS_END
