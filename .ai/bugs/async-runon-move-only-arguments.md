# Bug: `runOn` cannot forward move-only value arguments

- **Module**: `src/base/async`
- **Status**: Fixed
- **Files**: `ThreadPoolScheduler.hpp`, `tests/test_async/AsyncTest.cpp`

## Symptom

Passing a move-only value argument, such as a `std::unique_ptr`, to `runOn()` does not compile, even though the callable can take the argument by value.

## Root cause

`runOn(vn::ThreadPool&, F f, Args... args)` stores the callable and the arguments by value but, once the coroutine resumes, invokes them as `std::invoke(f, args...)`. The named locals are lvalues, so nothing can be moved into a callable that takes a move-only object by value. The return type is deduced as `std::invoke_result_t<F, Args...>` all the same, so the deduction and the actual value category disagree.

## Impact and verification

- A standalone minimal compile probe with a `std::unique_ptr` value argument confirms that the `runOn()` call fails to compile.
- The existing `runOn` tests do not cover a move-only value argument.
- The default-thread-pool overload has the same defect, because it forwards to the same invocation implementation.

## Fix direction

Invoke the stored callable and arguments with the value category the by-value API implies, add a regression test with a move-only argument, and verify ordinary value arguments, reference wrappers and both the void and non-void return paths.

## Fix

- `runOn()` invokes the frame's copies as `std::move(f), std::move(args)...`, matching the by-value semantics `std::invoke_result_t<F, Args...>` describes.
- `run()` forwards to the same implementation, so the default-thread-pool overload is fixed with it.

## Verification

- `ThreadPoolSchedulerTest.RunOnMovesMoveOnlyArgumentsIntoTheCallable`: a `std::unique_ptr` value argument compiles and runs.
- The existing `RunOnPool` / `RunOnPoolVoid` / `RunUsesDefaultPool` / `RunVoidUsesDefaultPool` cases still pass.
