#include <cstddef>
#include <new>

extern "C" void* luna_benchmark_allocate(std::size_t size) {
    return ::operator new(size);
}

extern "C" void luna_benchmark_deallocate(void* pointer) {
    ::operator delete(pointer);
}
