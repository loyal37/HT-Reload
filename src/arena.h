#pragma once

#include <cstddef>
#include <vector>

// Storage for tables that hold copies of engine pointers. A scan of the process for such pointers
// must not count its own bookkeeping, so each allocation here is a separate, recognisable region.
void* ArenaAllocate(size_t bytes);
void ArenaFree(void* block);
bool IsArenaAllocation(const void* allocationBase);

template <class T>
struct ArenaAllocator {
    using value_type = T;

    ArenaAllocator() = default;
    template <class U>
    ArenaAllocator(const ArenaAllocator<U>&) {}

    T* allocate(size_t count) { return static_cast<T*>(ArenaAllocate(count * sizeof(T))); }
    void deallocate(T* block, size_t) noexcept { ArenaFree(block); }

    template <class U>
    bool operator==(const ArenaAllocator<U>&) const { return true; }
};

template <class T>
using ArenaVector = std::vector<T, ArenaAllocator<T>>;
