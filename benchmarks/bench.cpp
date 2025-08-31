#include <iostream>
#include <chrono>
#include <vector>
#include "slab.hpp"

using namespace slab;

struct POD12
{
    int a, b, c;
};

template <class F>
long long bench_iters(const char *name, int iters, F &&fn)
{
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iters; ++i)
        fn();
    auto t1 = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    long long per = ns / (iters ? iters : 1);
    std::cout << name << ": " << per << " ns/op (" << iters << " iters)\n";
    return per;
}

int main()
{
    constexpr int ITERS = 500000;

    Cache c("pod12", sizeof(POD12));

    // --- Slab Alloc ---
    POD12 *tmp{};
    bench_iters("slab alloc", ITERS, [&]()
                { tmp = static_cast<POD12 *>(c.alloc()); });

    // --- Slab Free ---
    std::vector<void *> ps;
    ps.reserve(ITERS);
    for (int i = 0; i < ITERS; ++i)
        ps.push_back(c.alloc());
    bench_iters("slab free", ITERS, [&]()
                {
        c.free(ps.back());
        ps.pop_back(); });

    // --- operator new ---
    std::vector<POD12 *> new_ptrs;
    new_ptrs.reserve(ITERS);
    bench_iters("operator new", ITERS, [&]()
                { new_ptrs.push_back(new POD12); });

    // --- operator delete ---
    bench_iters("operator delete", ITERS, [&]()
                {
        delete new_ptrs.back();
        new_ptrs.pop_back(); });

    std::cout << "Done.\n";
    return 0;
}
