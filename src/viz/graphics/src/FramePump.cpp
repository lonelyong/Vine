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
        s_building_ = previous_;
        const std::lock_guard<std::mutex> gate(pump_->phase_mutex_);
        pump_->compiling_.store(false, std::memory_order_release);
    }
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

EngineFrame& FramePump::Build::frame() const noexcept
{
    return *frame_;
}

FramePump::Build FramePump::beginFrame()
{
    // A SECOND FRAME IS REFUSED, NOT QUEUED (see claimInterval): the interval is claimed BEFORE anything is read or
    // moved, so a caller on another thread is answered here - and the slot below is only rotated by the winner.
    if (!claimInterval()) {
        return Build{ s_building_ == this ? Build::Refusal::Reentrant : Build::Refusal::Concurrent };
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
    // A PREPARE CLAIMS THE SAME INTERVAL (see claimInterval): it walks the same tables, so the two rules - one owner,
    // refused not queued - are one rule, not two.
    if (!claimInterval()) {
        return Build{ s_building_ == this ? Build::Refusal::Reentrant : Build::Refusal::Concurrent };
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
        if (runner_ == nullptr) {
            // Inline: the calling thread IS the engine thread, so "hand it over" has nowhere else to go. The answer is
            // complete when this returns - which is what `frame()` reports as Ran.
            job();
            return;
        }
        runner_->submit(std::move(job));
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
    if (runner_ != nullptr) {
        runner_->waitIdle();
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
    if (runner_ == nullptr) {
        // Inline: every frame this pump handed out was answered before submit() returned, so a number it does not know
        // is simply not built yet.
        return completed_.load(std::memory_order_acquire) >= number;
    }
    (void)runner_->wait(timeout);
    return completed_.load(std::memory_order_acquire) >= number;
}

std::uint64_t FramePump::lastNumber() const noexcept
{
    return number_.load(std::memory_order_acquire);
}

bool FramePump::threaded() const noexcept
{
    return runner_ != nullptr;
}

bool FramePump::onEngineThread() const noexcept
{
    return runner_ != nullptr && runner_->onRunnerThread();
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

bool FramePump::setThreaded(bool threaded)
{
    if (threaded == (runner_ != nullptr)) {
        return true; // already there
    }
    if (onEngineThread()) {
        // A MODE CHANGE FROM INSIDE THE FRAME IS REFUSED, not half applied: stopping the runner from its own thread
        // cannot join it (see FrameRunner::stop), and dropping it here would leave a joinable thread for its destructor.
        // The caller may switch between frames, never from a frame body - and it is TOLD, rather than left to believe
        // the switch happened.
        return false;
    }
    if (threaded) {
        // The thread exists only when a host asked for one: an engine that never does allocates nothing and starts
        // nothing, and the inline path stays literally the same code.
        runner_ = std::make_unique<FrameRunner>(RunnerMode::Threaded);
        return true;
    }
    // Back to inline: a frame in flight is finished first (the thread is joined, not detached), so nothing is being
    // answered while the host goes on to describe frames here.
    runner_->waitIdle();
    runner_->stop();
    runner_.reset();
    return true;
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
    if (runner_ != nullptr) {
        runner_->waitIdle();
        runner_->stop(); // a call from the frame thread only asks it to leave (see FrameRunner::stop)
        if (onEngineThread()) {
            // THE JOIN BELONGS TO ANOTHER THREAD: the loop exits after the job it is inside, and the next stop() - from
            // the destructor or from shutdown() on the host's thread - performs it.
            return;
        }
        runner_.reset();
    }
    // NOTHING IS ANSWERING ANY MORE: the phase this pump owns is left where the next frame may start from, whether the
    // pump was stopped between frames or from its own frame thread (the frame it was inside has been answered by the
    // time its job returned).
    answering_.store(false, std::memory_order_release);
}

} // namespace detail

VN_GRAPHICS_NS_END
