#include "resolver.h"

#include <windows.h>

#include <algorithm>

#include "log.h"

namespace {

// Passed to LowLevelOutputDebugStringf by HandleMountPakDelegate and by the MountPaksEx handler,
// so it survives in shipping builds and is referenced from exactly those functions.
const wchar_t kMountAnchor[] = L"Mounting pak file: %s \n";

constexpr size_t kPage = 0x1000;
constexpr size_t kInstanceWords = 12;

bool Has(const std::vector<uintptr_t>& values, uintptr_t value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

// Remembers recently queried regions so that probing many heap pointers stays cheap and fault-free.
class ReadableCache {
public:
    bool Readable(uintptr_t address, size_t size) {
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

private:
    struct Entry {
        uintptr_t begin = 0;
        uintptr_t end = 0;
        bool readable = false;
    };
    Entry entries_[8];
    size_t next_ = 0;
};

bool PlausibleHeapPointer(const Image& image, uintptr_t value) {
    return value >= 0x10000 && value < 0x00007FFFFFFF0000 && (value & 7) == 0 && !image.Contains(value);
}

// A heap object whose vtable lives in the game image.
bool LooksLikeObject(const Image& image, ReadableCache& cache, uintptr_t object) {
    uintptr_t vtable = 0;
    uintptr_t firstMethod = 0;
    return PlausibleHeapPointer(image, object) && cache.Readable(object, sizeof vtable) &&
           SafeRead(object, &vtable, sizeof vtable) && image.Contains(vtable) && !Image::In(image.exec, vtable) &&
           SafeRead(vtable, &firstMethod, sizeof firstMethod) && Image::In(image.exec, firstMethod);
}

void LogBytes(const char* label, uintptr_t address, size_t size) {
    for (size_t offset = 0; offset < size; offset += 32) {
        uint8_t bytes[32];
        size_t count = std::min<size_t>(sizeof bytes, size - offset);
        if (!SafeRead(address + offset, bytes, count)) {
            Log("  %s +%03zx 不可读", label, offset);
            return;
        }
        Log("  %s +%03zx %s", label, offset, Hex(bytes, count).c_str());
    }
}

// The stock handler starts with `if (PakOrder == INDEX_NONE)`, i.e. a `cmp r32, -1` on its int32 argument.
bool ComparesWithIndexNone(uintptr_t function, size_t size) {
    uint8_t code[0x180];
    size = std::min(size, sizeof code);
    if (size < 3 || !SafeRead(function, code, size)) {
        return false;
    }
    for (size_t i = 0; i + 2 < size; ++i) {
        if (code[i] == 0x83 && (code[i + 1] & 0xF8) == 0xF8 && code[i + 2] == 0xFF) {
            return true;
        }
    }
    return false;
}

}  // namespace

// The generated code for UKismetSystemLibrary holds this name in two read-only tables:
// FClassFunctionLinkInfo { construct function, name }, whose functions are all the same size, and
// FNameNativePtrPair { name, exec thunk }, whose functions differ. The thunk follows the name in the latter.
uintptr_t FindCollectGarbageThunk(const Image& image) {
    static const char kName[] = "\0CollectGarbage";
    std::vector<uintptr_t> names = FindBytes(image.readOnly, kName, sizeof kName);
    if (names.size() != 1) {
        Log("垃圾回收: 函数名出现 %zu 次，需要正好 1 次", names.size());
        return 0;
    }
    uintptr_t name = names[0] + 1;

    std::vector<uintptr_t> thunks;
    for (uintptr_t entry : FindBytes(image.readOnly, &name, sizeof name)) {
        if (entry % sizeof(uintptr_t)) {
            continue;
        }
        size_t neighbours = 0;
        size_t firstSize = 0;
        bool sameSize = true;
        for (intptr_t k = -3; k <= 3; ++k) {
            uintptr_t function = 0;
            if (!SafeRead(entry + 8 + 16 * k, &function, sizeof function) || !Image::In(image.exec, function)) {
                continue;
            }
            uintptr_t functionEnd = 0;
            size_t size = FunctionStart(function, &functionEnd) == function ? functionEnd - function : 0;
            if (neighbours++ == 0) {
                firstSize = size;
            } else if (size != firstSize) {
                sameSize = false;
            }
        }
        bool constructTable = sameSize && firstSize != 0;
        uintptr_t thunk = 0;
        if (neighbours >= 3 && !constructTable && SafeRead(entry + 8, &thunk, sizeof thunk) &&
            Image::In(image.exec, thunk) && !Has(thunks, thunk)) {
            thunks.push_back(thunk);
        }
    }
    if (thunks.size() != 1) {
        Log("垃圾回收: 找到 %zu 个可能的入口，需要正好 1 个，本次运行不主动触发回收", thunks.size());
        return 0;
    }
    Log("垃圾回收: execCollectGarbage RVA %#zx", thunks[0] - image.base);
    LogBytes("入口", thunks[0], 0x40);
    return thunks[0];
}

bool Resolver::Step(const Image& image) {
    ++attempts_;
    bool verbose = attempts_ <= 3 || attempts_ % 20 == 0;
    // The code is unpacked while the game starts, so an early scan can miss one of the two
    // functions that use the anchor. Keep rescanning until both are known.
    if (candidates_.size() < 2 || attempts_ % 10 == 0) {
        FindCandidates(image, verbose);
    }
    return !candidates_.empty() && FindBoundInstance(image, verbose);
}

bool Resolver::FindCandidates(const Image& image, bool verbose) {
    std::vector<uintptr_t> strings = FindBytes(image.readOnly, kMountAnchor, sizeof kMountAnchor);
    if (strings.empty()) {
        strings = FindBytes(image.writable, kMountAnchor, sizeof kMountAnchor);
    }
    if (strings.size() != 1) {
        if (verbose) {
            Log("定位(第%d次): 锚点字符串出现 %zu 次，需要正好 1 次", attempts_, strings.size());
        }
        return false;
    }

    size_t faultedPages = 0;
    std::vector<uintptr_t> refs = FindLeaRefs(image.exec, strings[0], &faultedPages);
    std::vector<uintptr_t> functions;
    for (uintptr_t ref : refs) {
        uintptr_t function = FunctionStart(ref);
        if (function && !Has(functions, function)) {
            functions.push_back(function);
        }
    }
    if (functions.empty()) {
        if (verbose) {
            Log("定位(第%d次): 锚点 RVA %#zx 有 %zu 处引用，但没有可用函数（代码段可能尚未解密，不可读页 %zu）",
                attempts_, strings[0] - image.base, refs.size(), faultedPages);
        }
        return false;
    }

    std::sort(functions.begin(), functions.end());
    if (functions == candidates_) {
        return true;
    }
    candidates_ = functions;
    Log("定位: 锚点 RVA %#zx，引用 %zu 处，候选函数 %zu 个", strings[0] - image.base, refs.size(), functions.size());
    for (uintptr_t function : candidates_) {
        uintptr_t functionEnd = 0;
        FunctionStart(function, &functionEnd);
        Log("  候选函数 RVA %#zx 大小 %#zx", function - image.base, functionEnd - function);
    }
    return true;
}

// FCoreDelegates::MountPak is a global whose heap-allocated delegate instance stores
// { FPakPlatformFile* UserObject; MethodPtr } next to each other. Finding that pair identifies
// both the handler and the object without assuming any struct offsets.
bool Resolver::FindBoundInstance(const Image& image, bool verbose) {
    ReadableCache cache;
    size_t hits = 0;
    auto consider = [&](uintptr_t slot, uintptr_t storage, const uintptr_t* words, size_t index) {
        if (index == 0 || !Has(candidates_, words[index]) || !LooksLikeObject(image, cache, words[index - 1])) {
            return;
        }
        // The probe window can run past a small neighbouring allocation and see the same instance again.
        if (hits > 0 && api_.handleMountPak == words[index] && api_.pakPlatformFile == words[index - 1]) {
            return;
        }
        if (++hits == 1) {
            api_.handleMountPak = words[index];
            api_.pakPlatformFile = words[index - 1];
        }
        Log("  绑定: 全局 RVA %#zx -> 实例 %#zx 偏移 %#zx，函数 RVA %#zx，对象 %#zx", slot - image.base, storage,
            index * sizeof(uintptr_t), words[index] - image.base, words[index - 1]);
    };

    for (const Region& region : image.writable) {
        for (uintptr_t page = region.begin; page < region.end; page += kPage) {
            uintptr_t slots[kPage / sizeof(uintptr_t)];
            size_t count = std::min<size_t>(kPage, region.end - page) / sizeof(uintptr_t);
            if (!SafeRead(page, slots, count * sizeof(uintptr_t))) {
                continue;
            }
            for (size_t i = 0; i < count; ++i) {
                uintptr_t slot = page + i * sizeof(uintptr_t);
                consider(slot, slot, slots, i);  // delegate with inline storage
                uintptr_t instance = slots[i];
                uintptr_t words[kInstanceWords];
                if (!PlausibleHeapPointer(image, instance) || !cache.Readable(instance, sizeof words) ||
                    !SafeRead(instance, words, sizeof words)) {
                    continue;
                }
                for (size_t k = 1; k < kInstanceWords; ++k) {
                    consider(slot, instance, words, k);
                }
            }
        }
    }

    if (hits == 0) {
        if (verbose) {
            Log("定位(第%d次): 尚未找到绑定到候选函数的挂载委托（引擎可能还没初始化完）", attempts_);
        }
        return false;
    }
    if (hits > 1) {
        Log("警告: 找到 %zu 处不同的绑定，使用第一处", hits);
    }

    uintptr_t functionEnd = 0;
    FunctionStart(api_.handleMountPak, &functionEnd);
    size_t size = std::min<size_t>(functionEnd > api_.handleMountPak ? functionEnd - api_.handleMountPak : 0, 0x180);
    Log("定位完成: HandleMountPakDelegate RVA %#zx，FPakPlatformFile %#zx", api_.handleMountPak - image.base,
        api_.pakPlatformFile);
    LogBytes("函数", api_.handleMountPak, size);
    if (!ComparesWithIndexNone(api_.handleMountPak, size)) {
        Log("警告: 函数里没看到与 INDEX_NONE 的比较，参数形式可能和 UE 5.6 原版不同");
    }
    return true;
}
