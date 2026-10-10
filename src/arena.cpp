#include "arena.h"

#include <windows.h>

#include <new>

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
void* g_blocks[256];

}  // namespace

void* ArenaAllocate(size_t bytes) {
    void* block = VirtualAlloc(nullptr, bytes ? bytes : 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!block) {
        throw std::bad_alloc();
    }
    AcquireSRWLockExclusive(&g_lock);
    bool stored = false;
    for (void*& slot : g_blocks) {
        if (!slot) {
            slot = block;
            stored = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!stored) {
        VirtualFree(block, 0, MEM_RELEASE);
        throw std::bad_alloc();
    }
    return block;
}

void ArenaFree(void* block) {
    if (!block) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    for (void*& slot : g_blocks) {
        if (slot == block) {
            slot = nullptr;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    VirtualFree(block, 0, MEM_RELEASE);
}

bool IsArenaAllocation(const void* allocationBase) {
    bool found = false;
    AcquireSRWLockShared(&g_lock);
    for (void* slot : g_blocks) {
        if (slot == allocationBase) {
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}
