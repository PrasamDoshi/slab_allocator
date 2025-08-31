# Slab Allocator

This project implements a simple **slab allocator** in C++.  
A slab allocator is a memory management technique that pre-allocates memory chunks ("slabs") for objects of fixed size, improving allocation speed and reducing fragmentation.  


---

```bash
make
make run-tests
make run-bench
```


## Benchmark Results
```bash
slab alloc:     10 ns/op   (500000 iters)
slab free:      8 ns/op    (500000 iters)
operator new:   24 ns/op   (500000 iters)
operator delete:27 ns/op   (500000 iters)
```