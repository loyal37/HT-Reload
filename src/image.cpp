#include "image.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace {

constexpr uintptr_t kPage = 0x1000;
constexpr size_t kMaxHitsPerBlock = 32;

size_t CopyNoFault(uintptr_t address, void* out, size_t size) {
    __try {
        memcpy(out, reinterpret_cast<const void*>(address), size);
        return size;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

size_t FindBlock(const uint8_t* begin, const uint8_t* end, const uint8_t* pattern, size_t size, uintptr_t* out) {
    size_t count = 0;
    __try {
        for (const uint8_t* p = begin; p < end; ++p) {
            p = static_cast<const uint8_t*>(memchr(p, pattern[0], static_cast<size_t>(end - p)));
            if (!p) {
                break;
            }
            if (memcmp(p, pattern, size) == 0 && count < kMaxHitsPerBlock) {
                out[count++] = reinterpret_cast<uintptr_t>(p);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

size_t ScanLeaBlock(const uint8_t* begin, const uint8_t* end, uintptr_t target, uintptr_t* out, bool* faulted) {
    size_t count = 0;
    __try {
        for (const uint8_t* p = begin; p < end; ++p) {
            int32_t disp;
            memcpy(&disp, p, sizeof disp);
            if (reinterpret_cast<uintptr_t>(p + 4) + static_cast<intptr_t>(disp) != target) {
                continue;
            }
            // REX.W 8D /r with mod=00 rm=101
            if ((p[-3] & 0xFB) == 0x48 && p[-2] == 0x8D && (p[-1] & 0xC7) == 0x05 && count < kMaxHitsPerBlock) {
                out[count++] = reinterpret_cast<uintptr_t>(p);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
    }
    return count;
}

size_t ScanBranchBlock(const uint8_t* begin, const uint8_t* end, uintptr_t target, uintptr_t* out) {
    size_t count = 0;
    __try {
        for (const uint8_t* p = begin; p < end; ++p) {
            int32_t disp;
            memcpy(&disp, p, sizeof disp);
            if (reinterpret_cast<uintptr_t>(p + 4) + static_cast<intptr_t>(disp) == target && (p[-1] == 0xE8 || p[-1] == 0xE9) &&
                count < kMaxHitsPerBlock) {
                out[count++] = reinterpret_cast<uintptr_t>(p);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

void Append(std::vector<Region>& regions, Region region) {
    if (!regions.empty() && regions.back().end == region.begin) {
        regions.back().end = region.end;
    } else {
        regions.push_back(region);
    }
}

}  // namespace

bool Image::Open() {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    base = reinterpret_cast<uintptr_t>(dos);
    end = base + nt->OptionalHeader.SizeOfImage;
    return true;
}

void Image::Refresh() {
    exec.clear();
    readOnly.clear();
    writable.clear();
    for (uintptr_t address = base; address < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof info)) {
            break;
        }
        uintptr_t regionEnd = std::min(reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize, end);
        if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            Region region{address, regionEnd};
            if (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) {
                Append(exec, region);
            } else if (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) {
                Append(writable, region);
            } else if (info.Protect & PAGE_READONLY) {
                Append(readOnly, region);
            }
        }
        address = regionEnd;
    }
}

bool Image::In(const std::vector<Region>& regions, uintptr_t address) {
    for (const Region& region : regions) {
        if (address >= region.begin && address < region.end) {
            return true;
        }
    }
    return false;
}

bool SafeRead(uintptr_t address, void* out, size_t size) {
    return CopyNoFault(address, out, size) == size;
}

bool PlausibleHeapPointer(const Image& image, uintptr_t value) {
    return value >= 0x10000 && value < 0x00007FFFFFFF0000 && (value & 7) == 0 && !image.Contains(value);
}

bool ReadableCache::Readable(uintptr_t address, size_t size) {
    for (const Entry& entry : entries_) {
        if (address >= entry.begin && address < entry.end) {
            return entry.readable && address + size <= entry.end;
        }
    }
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof info)) {
        return false;
    }
    Entry& entry = entries_[next_++ % std::size(entries_)];
    entry.begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
    entry.end = entry.begin + info.RegionSize;
    entry.readable = info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS));
    return entry.readable && address + size <= entry.end;
}

std::vector<uintptr_t> FindBytes(const std::vector<Region>& regions, const void* pattern, size_t size) {
    constexpr uintptr_t kBlock = 0x10000;
    std::vector<uintptr_t> hits;
    for (const Region& region : regions) {
        if (region.end - region.begin < size) {
            continue;
        }
        uintptr_t last = region.end - size + 1;
        for (uintptr_t begin = region.begin; begin < last; begin += kBlock) {
            uintptr_t found[kMaxHitsPerBlock];
            size_t count = FindBlock(reinterpret_cast<const uint8_t*>(begin),
                                     reinterpret_cast<const uint8_t*>(std::min(last, begin + kBlock)),
                                     static_cast<const uint8_t*>(pattern), size, found);
            hits.insert(hits.end(), found, found + count);
        }
    }
    return hits;
}

std::vector<uintptr_t> FindLeaRefs(const std::vector<Region>& regions, uintptr_t target, size_t* faultedPages) {
    std::vector<uintptr_t> refs;
    size_t faults = 0;
    for (const Region& region : regions) {
        if (region.end - region.begin < 8) {
            continue;
        }
        uintptr_t last = region.end - 4;
        for (uintptr_t begin = region.begin + 3; begin < last;) {
            uintptr_t blockEnd = std::min(last, (begin + kPage) & ~(kPage - 1));
            uintptr_t found[kMaxHitsPerBlock];
            bool faulted = false;
            size_t count = ScanLeaBlock(reinterpret_cast<const uint8_t*>(begin),
                                        reinterpret_cast<const uint8_t*>(blockEnd), target, found, &faulted);
            refs.insert(refs.end(), found, found + count);
            faults += faulted;
            begin = blockEnd;
        }
    }
    if (faultedPages) {
        *faultedPages = faults;
    }
    return refs;
}

std::vector<uintptr_t> FindBranchRefs(const std::vector<Region>& regions, uintptr_t target) {
    std::vector<uintptr_t> refs;
    for (const Region& region : regions) {
        if (region.end - region.begin < 8) {
            continue;
        }
        uintptr_t last = region.end - 4;
        for (uintptr_t begin = region.begin + 1; begin < last;) {
            uintptr_t blockEnd = std::min(last, (begin + kPage) & ~(kPage - 1));
            uintptr_t found[kMaxHitsPerBlock];
            size_t count = ScanBranchBlock(reinterpret_cast<const uint8_t*>(begin), reinterpret_cast<const uint8_t*>(blockEnd),
                                           target, found);
            refs.insert(refs.end(), found, found + count);
            begin = blockEnd;
        }
    }
    return refs;
}

uintptr_t FunctionStart(uintptr_t pc, uintptr_t* functionEnd) {
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
    RUNTIME_FUNCTION function{};
    if (!entry || !SafeRead(reinterpret_cast<uintptr_t>(entry), &function, sizeof function)) {
        return 0;
    }
    // A function split into several ranges chains back to its primary entry.
    for (int depth = 0; depth < 8; ++depth) {
        if (function.UnwindData & 1) {
            if (!SafeRead(imageBase + (function.UnwindData & ~1u), &function, sizeof function)) {
                return 0;
            }
            continue;
        }
        uint8_t header[4];
        if (!SafeRead(imageBase + function.UnwindData, header, sizeof header)) {
            return 0;
        }
        if (!((header[0] >> 3) & UNW_FLAG_CHAININFO)) {
            break;
        }
        size_t codeSlots = (header[2] + 1u) & ~1u;
        if (!SafeRead(imageBase + function.UnwindData + 4 + codeSlots * 2, &function, sizeof function)) {
            return 0;
        }
    }
    if (functionEnd) {
        *functionEnd = imageBase + function.EndAddress;
    }
    return imageBase + function.BeginAddress;
}
