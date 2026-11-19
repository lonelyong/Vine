// A started cache with static storage duration must not abort at exit.
//
// The shared sweeper used to be a joinable `inline static std::thread` member,
// constructed on the first start() and therefore destroyed before this object.
// std::thread's destructor calls std::terminate when the thread is joinable, so
// the process aborted at exit (SIGABRT). The sweeper is process-lifetime state
// with a detached worker now; running this program to completion is the check.

#include <vine/runtime/InMemoryCache.hpp>

#include <chrono>
#include <thread>

namespace
{

vn::runtime::InMemoryCache<int, int> g_cache;

} // namespace

int main()
{
    g_cache.start(std::chrono::milliseconds(1));
    g_cache.set(1, 1, std::chrono::seconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return 0;
}
