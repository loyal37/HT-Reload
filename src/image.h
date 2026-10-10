#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct Region {
    uintptr_t begin;
    uintptr_t end;
};

// Committed memory of the game's main module, grouped by page protection.
// The game's code is packed on disk, so everything is read from the live process.
struct Image {
    uintptr_t base = 0;
    uintptr_t end = 0;
    std::vector<Region> exec;
    std::vector<Region> readOnly;
    std::vector<Region> writable;

    bool Open();
    void Refresh();

    bool Contains(uintptr_t address) const { return address >= base && address < end; }
    static bool In(const std::vector<Region>& regions, uintptr_t address);
};

bool SafeRead(uintptr_t address, void* out, size_t size);

// True for values that could point at heap memory: user-mode, 8-byte aligned, outside the image.
bool PlausibleHeapPointer(const Image& image, uintptr_t value);

// Remembers recently queried regions so that probing many pointers stays cheap and fault-free.
class ReadableCache {
public:
    bool Readable(uintptr_t address, size_t size);

private:
    struct Entry {
        uintptr_t begin = 0;
        uintptr_t end = 0;
        bool readable = false;
    };
    Entry entries_[8];
    size_t next_ = 0;
};

// Addresses of every occurrence of `pattern`.
std::vector<uintptr_t> FindBytes(const std::vector<Region>& regions, const void* pattern, size_t size);

// Addresses of the disp32 field of every `lea r64, [rip+disp32]` that resolves to `target`.
std::vector<uintptr_t> FindLeaRefs(const std::vector<Region>& regions, uintptr_t target, size_t* faultedPages);

// Addresses of the rel32 field of every `call` or `jmp` that goes straight to `target`.
std::vector<uintptr_t> FindBranchRefs(const std::vector<Region>& regions, uintptr_t target);

// Start of the function containing `pc`, from the module's unwind tables. 0 when unknown.
uintptr_t FunctionStart(uintptr_t pc, uintptr_t* functionEnd = nullptr);
