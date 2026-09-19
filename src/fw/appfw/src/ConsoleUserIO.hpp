#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

#include <vine/async/AsyncEvent.hpp>
#include <vine/appfw/ConsoleProgressReporter.hpp>
#include <vine/appfw/UserIO.hpp>

V_APPFW_NS_BEGIN

/**
 * @brief Headless UserIO: writes to stdout and reads from stdin.
 *
 * Each getXxxAsync() prints the prompt and yields the next line of stdin. The
 * reading itself happens on one background thread, because std::getline blocks and
 * cannot be interrupted: a command awaiting the line therefore stays cancellable,
 * so UserIO::cancelPendingInput() - and with it Application::shutdown() - makes the
 * read return std::nullopt instead of leaving that command parked, to be resumed
 * into a destroyed manager once the teardown is over.
 *
 * Lines typed while nothing was waiting are buffered and handed to the next read.
 * Only one read waits at a time, like every other implementation (see UserIO).
 *
 * @note Exported although the class is appfw-private (this header lives in src/, not in sdk/): the headless progress
 * test constructs it directly to check what it writes to stdout, which needs the symbols in the shared library.
 */
class V_APPFW_API ConsoleUserIO : public UserIO {
    V_OBJECT_META_DECL;
    V_DISABLE_COPY_MOVE(ConsoleUserIO);

  public:
    ConsoleUserIO();
    ~ConsoleUserIO() override;

  public:
    void putString(const String& str) override;
    void clear() override;
    void cancelPendingInput() override;

    vine::async::Task<std::optional<String>>        getStringAsync(const String& prompt = {}) override;
    vine::async::Task<std::optional<int>>           getIntAsync(const String& prompt = {}) override;
    vine::async::Task<std::optional<double>>        getDoubleAsync(const String& prompt = {}) override;
    vine::async::Task<std::optional<math::Point3d>> getPoint3dAsync(const String& prompt = {}) override;

  private:
    /// State shared with the background reader thread. Both sides hold it through a
    /// shared_ptr, so the thread may still be blocked in std::getline after the
    /// ConsoleUserIO is gone without touching it.
    struct StdinReader;

    /// Releases the interaction slot however the awaiting read ends, including a
    /// coroutine frame that is destroyed before it ever resumed.
    struct ReadScope {
        ConsoleUserIO* self;
        ~ReadScope() { self->endRead(); }
    };

    /// Claims the interaction slot, starting the reader thread on first use; false
    /// when another read is already waiting.
    bool beginRead();
    /// Releases the interaction slot.
    void endRead() noexcept;
    /// Waits for the next line of stdin; std::nullopt when the read was cancelled
    /// or the stream ended.
    vine::async::Task<std::optional<String>> readLineAsync(const String& prompt);
    /// Writes one line of output.
    ///
    /// Non-virtual on purpose: the progress reporter is a member, so it is still
    /// writing while the rest of this object is being destroyed - a virtual call
    /// there would dispatch to UserIO::putString(), which is pure.
    void writeLine(const String& str);

  private:
    std::shared_ptr<StdinReader> reader_;
    /// Guards the interaction slot and the reader's start flag.
    std::mutex                   state_mutex_;
    /// True while a read waits or unwinds; guarded by state_mutex_.
    bool                         slot_busy_{ false };
    /// Guards stdout so concurrent writers cannot interleave half a line.
    std::mutex                   output_mutex_;
    /// Prints the progress of a LongRunning command as throttled console lines:
    /// the headless counterpart of the GUI presenter, and the only consumer of
    /// the ambient progress registry in a host without a GUI.
    ///
    /// Declared last so it is destroyed first, while output_mutex_ is still usable.
    std::unique_ptr<ConsoleProgressReporter> progress_;
};

V_APPFW_NS_END
