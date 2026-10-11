#include "resolver.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "log.h"

namespace {

// Passed to LowLevelOutputDebugStringf by HandleMountPakDelegate and by the MountPaksEx handler,
// so it survives in shipping builds and is referenced from exactly those functions.
const wchar_t kMountAnchor[] = L"Mounting pak file: %s \n";
// The same for HandleUnmountPakDelegate.
const wchar_t kUnmountAnchor[] = L"Unmounting pak file: %s \n";

constexpr size_t kPage = 0x1000;
constexpr size_t kInstanceWords = 12;

// Calls visit(slot, storage, words, index) for every word of the image's writable data and of the
// heap block such a word points at. Those are the places where a global delegate keeps the
// { object, method } pair it is bound to: inline, or in a heap-allocated instance.
template <class Visit>
void ForEachInstanceWord(const Image& image, ReadableCache& cache, Visit&& visit) {
    for (const Region& region : image.writable) {
        for (uintptr_t page = region.begin; page < region.end; page += kPage) {
            uintptr_t slots[kPage / sizeof(uintptr_t)];
            size_t count = std::min<size_t>(kPage, region.end - page) / sizeof(uintptr_t);
            if (!SafeRead(page, slots, count * sizeof(uintptr_t))) {
                continue;
            }
            for (size_t i = 0; i < count; ++i) {
                uintptr_t slot = page + i * sizeof(uintptr_t);
                visit(slot, slot, slots, i);  // delegate with inline storage
                uintptr_t instance = slots[i];
                uintptr_t words[kInstanceWords];
                if (!PlausibleHeapPointer(image, instance) || !cache.Readable(instance, sizeof words) ||
                    !SafeRead(instance, words, sizeof words)) {
                    continue;
                }
                for (size_t k = 1; k < kInstanceWords; ++k) {
                    visit(slot, instance, words, k);
                }
            }
        }
    }
}

bool Has(const std::vector<uintptr_t>& values, uintptr_t value) {
    return std::find(values.begin(), values.end(), value) != values.end();
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
            Log("  %s +%03zx unreadable", label, offset);
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
        Log("GC: the function name occurs %zu times, exactly 1 is needed", names.size());
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
        Log("GC: found %zu candidate entries, exactly 1 is needed; no collections will be requested in this run", thunks.size());
        return 0;
    }
    Log("GC: execCollectGarbage RVA %#zx", thunks[0] - image.base);
    LogBytes("entry", thunks[0], 0x40);
    return thunks[0];
}

uintptr_t FindEngineGlobal(uintptr_t collectGarbageThunk) {
    uint8_t code[0x40];
    if (!collectGarbageThunk || !SafeRead(collectGarbageThunk, code, sizeof code)) {
        return 0;
    }
    for (size_t i = 0; i + 7 <= sizeof code; ++i) {
        if (code[i] == 0x48 && code[i + 1] == 0x8B && code[i + 2] == 0x0D) {
            int32_t displacement;
            memcpy(&displacement, code + i + 3, sizeof displacement);
            return collectGarbageThunk + i + 7 + displacement;
        }
    }
    return 0;
}

// FCoreDelegates::OnUnmountPak is bound to the same object as MountPak, in the same way, so the
// handler is the function that uses the anchor string and sits next to that object in a delegate instance.
uintptr_t FindUnmountHandler(const Image& image, uintptr_t pakPlatformFile) {
    std::vector<uintptr_t> strings = FindBytes(image.readOnly, kUnmountAnchor, sizeof kUnmountAnchor);
    if (strings.size() != 1) {
        Log("Unmount: the anchor string occurs %zu times, exactly 1 is needed; old containers will not be unmounted in this run", strings.size());
        return 0;
    }
    std::vector<uintptr_t> candidates;
    for (uintptr_t ref : FindLeaRefs(image.exec, strings[0], nullptr)) {
        uintptr_t function = FunctionStart(ref);
        if (function && !Has(candidates, function)) {
            candidates.push_back(function);
        }
    }
    std::vector<uintptr_t> bound;
    ReadableCache cache;
    ForEachInstanceWord(image, cache, [&](uintptr_t, uintptr_t, const uintptr_t* words, size_t index) {
        if (index > 0 && words[index - 1] == pakPlatformFile && Has(candidates, words[index]) && !Has(bound, words[index])) {
            bound.push_back(words[index]);
        }
    });
    if (bound.size() != 1) {
        Log("Unmount: %zu functions use the anchor, %zu of them bound to the mount object, exactly 1 is needed; old containers will not be unmounted in this run", candidates.size(),
            bound.size());
        return 0;
    }
    uintptr_t functionEnd = 0;
    FunctionStart(bound[0], &functionEnd);
    Log("Unmount: HandleUnmountPakDelegate RVA %#zx size %#zx", bound[0] - image.base, functionEnd - bound[0]);
    LogBytes("code", bound[0], std::min<size_t>(functionEnd > bound[0] ? functionEnd - bound[0] : 0, 0x80));
    return bound[0];
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
            Log("Locate (attempt %d): the anchor string occurs %zu times, exactly 1 is needed", attempts_, strings.size());
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
            Log("Locate (attempt %d): anchor RVA %#zx has %zu references but no usable function (the code may not be unpacked yet; unreadable pages %zu)",
                attempts_, strings[0] - image.base, refs.size(), faultedPages);
        }
        return false;
    }

    std::sort(functions.begin(), functions.end());
    if (functions == candidates_) {
        return true;
    }
    candidates_ = functions;
    Log("Locate: anchor RVA %#zx, %zu references, %zu candidate functions", strings[0] - image.base, refs.size(), functions.size());
    for (uintptr_t function : candidates_) {
        uintptr_t functionEnd = 0;
        FunctionStart(function, &functionEnd);
        Log("  candidate RVA %#zx size %#zx", function - image.base, functionEnd - function);
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
        Log("  binding: global RVA %#zx -> instance %#zx offset %#zx, function RVA %#zx, object %#zx", slot - image.base, storage,
            index * sizeof(uintptr_t), words[index] - image.base, words[index - 1]);
    };

    ForEachInstanceWord(image, cache, consider);

    if (hits == 0) {
        if (verbose) {
            Log("Locate (attempt %d): no mount delegate bound to a candidate yet (the engine may still be initialising)", attempts_);
        }
        return false;
    }
    if (hits > 1) {
        Log("Warning: found %zu different bindings, using the first", hits);
    }

    uintptr_t functionEnd = 0;
    FunctionStart(api_.handleMountPak, &functionEnd);
    size_t size = std::min<size_t>(functionEnd > api_.handleMountPak ? functionEnd - api_.handleMountPak : 0, 0x180);
    Log("Located: HandleMountPakDelegate RVA %#zx, FPakPlatformFile %#zx", api_.handleMountPak - image.base,
        api_.pakPlatformFile);
    LogBytes("code", api_.handleMountPak, size);
    if (!ComparesWithIndexNone(api_.handleMountPak, size)) {
        Log("Warning: the function has no comparison with INDEX_NONE; its parameters may differ from stock UE 5.6");
    }
    return true;
}
