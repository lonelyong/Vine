
#include <chrono>
#include <gtest/gtest.h>
#include <unordered_set>
#include <vector>

TEST(TestPerf, VectorVsUnorderedSet)
{
    constexpr size_t N = 100000000;

    std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
    std::unordered_set<int>                        set;
    set.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        set.insert(i);
    }
    std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
    std::vector<int>                               vec;
    vec.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        vec.push_back(i);
    }
    std::chrono::high_resolution_clock::time_point t3 = std::chrono::high_resolution_clock::now();

    auto duration_set = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
    auto duration_vec = std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();

    printf("unordered_set insert duration: %f ms\n", duration_set * 1e-6);
    printf("vector insert duration: %f ms\n", duration_vec * 1e-6);
}