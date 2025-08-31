#include <iostream>
#include <cassert>
#include <vector>
#include "slab.hpp"

using namespace slab;

struct Test12 {
    int a,b,c;
};

static void test_cache_create() {
    Cache c("test", sizeof(Test12));
    assert(c.effective_size() >= sizeof(Test12));
}

static void test_alloc_basic() {
    Cache c("test", sizeof(Test12));
    auto* p1 = static_cast<Test12*>(c.alloc());
    p1->a = p1->b = p1->c = 1;
    auto* p2 = static_cast<Test12*>(c.alloc());
    p2->a = p2->b = p2->c = 2;
    c.free(p1);
    c.free(p2);
}

static void test_big_object() {
    Cache c("big", 1000);
    std::vector<void*> ptrs;
    for (int i=0; i<9; ++i) ptrs.push_back(c.alloc());
    for (void* p: ptrs) c.free(p);
}

static void test_typed_cache() {
    TypedCache<Test12> tc("typed");
    Test12* t = tc.alloc();
    assert(t->a == 0 && t->b == 0 && t->c == 0);
    tc.free(t);
}

int main() {
    test_cache_create();
    test_alloc_basic();
    test_big_object();
    test_typed_cache();

    std::cout << "ALL TESTS PASSED!\n";
    return 0;
}
