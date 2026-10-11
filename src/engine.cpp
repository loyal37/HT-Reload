#include "engine.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {

// Logged by UObject::Rename, and by nothing else.
const wchar_t kRenameAnchor[] = L"Renaming an object (%s) on top of an existing object (%s) is not allowed";

constexpr size_t kMaxVirtuals = 200;
constexpr uint32_t kFuncNative = 0x400;

// ERenameFlags: no loader reset, do not dirty, no redirector, not transactional. The same set the
// engine's own ReloadPackage uses for the package it is about to replace.
constexpr uint32_t kRenameFlags = 0x0001 | 0x0004 | 0x0010 | 0x0020;

using ProcessEventFn = void (*)(void* object, void* function, void* params);
using RenameFn = bool (*)(void* object, const wchar_t* newName, void* newOuter, uint32_t flags);

void Append(std::string& out, const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (length > 0) {
        out.append(text, std::min<size_t>(static_cast<size_t>(length), sizeof text - 1));
        out += "\r\n";
    }
}

uintptr_t VirtualAt(uintptr_t object, size_t slot) {
    uintptr_t table = 0;
    uintptr_t entry = 0;
    return SafeRead(object, &table, sizeof table) && SafeRead(table + slot * sizeof entry, &entry, sizeof entry) ? entry : 0;
}

// UObject::ProcessEvent begins by testing Function->FunctionFlags for FUNC_Native:
// `test dword ptr [rdx+disp32], 400h`. Returns the displacement, or 0.
size_t NativeFlagTest(uintptr_t function) {
    uint8_t code[0x80];
    if (!SafeRead(function, code, sizeof code)) {
        return 0;
    }
    for (size_t i = 0; i + 10 <= sizeof code; ++i) {
        uint32_t displacement;
        uint32_t immediate;
        memcpy(&displacement, code + i + 2, sizeof displacement);
        memcpy(&immediate, code + i + 6, sizeof immediate);
        if (code[i] == 0xF7 && code[i + 1] == 0x82 && immediate == kFuncNative && displacement < 0x400) {
            return displacement;
        }
    }
    return 0;
}

}  // namespace

bool EngineCalls::Resolve(const Image& image, const ObjectSystem& system, std::string& report) {
    *this = EngineCalls{};

    // UE_LOG keeps the format string in a static record; the function takes the record's address.
    uintptr_t rename = 0;
    for (uintptr_t text : FindBytes(image.readOnly, kRenameAnchor, sizeof kRenameAnchor)) {
        for (const std::vector<Region>* regions : {&image.readOnly, &image.writable}) {
            for (uintptr_t record : FindBytes(*regions, &text, sizeof text)) {
                size_t faulted = 0;
                for (uintptr_t ref : record % sizeof(uintptr_t) ? std::vector<uintptr_t>() : FindLeaRefs(image.exec, record, &faulted)) {
                    rename = FunctionStart(ref);
                }
            }
        }
    }
    if (!rename) {
        Append(report, "UObject::Rename not found");
        return false;
    }

    // Most classes inherit both functions, so the slot is the one most objects agree on.
    ArenaVector<ObjectInfo> objects = system.Snapshot();
    std::map<size_t, int> renameVotes;
    std::map<size_t, int> processVotes;
    std::map<size_t, int> offsetVotes;
    size_t step = std::max<size_t>(1, objects.size() / 400);
    for (size_t i = 0; i < objects.size(); i += step) {
        uintptr_t table = 0;
        uintptr_t entries[kMaxVirtuals];
        if (!SafeRead(objects[i].address, &table, sizeof table) || !SafeRead(table, entries, sizeof entries)) {
            continue;
        }
        for (size_t slot = 0; slot < kMaxVirtuals && Image::In(image.exec, entries[slot]); ++slot) {
            if (entries[slot] == rename) {
                ++renameVotes[slot];
            }
            if (size_t offset = NativeFlagTest(entries[slot])) {
                ++processVotes[slot];
                ++offsetVotes[offset];
                break;
            }
        }
    }
    auto winner = [](const std::map<size_t, int>& votes) {
        size_t best = 0;
        int count = 0;
        for (const auto& [value, number] : votes) {
            if (number > count) {
                best = value;
                count = number;
            }
        }
        return best;
    };
    renameSlot = winner(renameVotes);
    processEventSlot = winner(processVotes);
    functionFlagsOffset = winner(offsetVotes);
    Append(report, "UObject::Rename RVA %#zx, vtable slot %zu (%d sampled objects agree)", rename - image.base, renameSlot,
           renameVotes.count(renameSlot) ? renameVotes[renameSlot] : 0);
    Append(report, "UObject::ProcessEvent vtable slot %zu (%d sampled objects agree), UFunction::FunctionFlags offset %#zx", processEventSlot,
           processVotes.count(processEventSlot) ? processVotes[processEventSlot] : 0, functionFlagsOffset);
    return Ready();
}

uint16_t EngineCalls::ParamsSize(uintptr_t function) const {
    // uint32 FunctionFlags; uint8 NumParms; uint16 ParmsSize; uint16 ReturnValueOffset;
    uint16_t size = 0;
    return SafeRead(function + functionFlagsOffset + 6, &size, sizeof size) ? size : 0;
}

uintptr_t FindFunction(const ObjectSystem& system, const ArenaVector<ObjectInfo>& objects, const char* className,
                       const char* functionName) {
    auto find = [&](uintptr_t address) -> const ObjectInfo* {
        auto it = std::lower_bound(objects.begin(), objects.end(), address,
                                   [](const ObjectInfo& object, uintptr_t value) { return object.address < value; });
        return it != objects.end() && it->address == address ? &*it : nullptr;
    };
    for (const ObjectInfo& object : objects) {
        if (object.nameNumber != 0 || system.Name(object.nameId, 0) != functionName) {
            continue;
        }
        const ObjectInfo* klass = find(object.klass);
        const ObjectInfo* outer = find(object.outer);
        if (klass && outer && system.Name(klass->nameId, klass->nameNumber) == "Function" &&
            system.Name(outer->nameId, outer->nameNumber) == className) {
            return object.address;
        }
    }
    return 0;
}

bool CallProcessEvent(const EngineCalls& calls, uintptr_t object, uintptr_t function, void* params, DWORD* exceptionCode) {
    uintptr_t entry = VirtualAt(object, calls.processEventSlot);
    if (!entry) {
        *exceptionCode = ERROR_INVALID_ADDRESS;
        return false;
    }
    __try {
        reinterpret_cast<ProcessEventFn>(entry)(reinterpret_cast<void*>(object), reinterpret_cast<void*>(function), params);
        return true;
    } __except (*exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallRename(const EngineCalls& calls, uintptr_t object, const wchar_t* newName, bool* renamed, DWORD* exceptionCode) {
    uintptr_t entry = VirtualAt(object, calls.renameSlot);
    if (!entry) {
        *exceptionCode = ERROR_INVALID_ADDRESS;
        return false;
    }
    __try {
        *renamed = reinterpret_cast<RenameFn>(entry)(reinterpret_cast<void*>(object), newName, nullptr, kRenameFlags);
        return true;
    } __except (*exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
