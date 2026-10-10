#include "probe.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>

#include "arena.h"
#include "cityhash.h"
#include "image.h"
#include "log.h"

namespace {

constexpr uint32_t kInternalRootSet = 1u << 30;  // EInternalObjectFlags::RootSet
constexpr uint8_t kChunkTypeExportBundleData = 1;
constexpr size_t kMaxHits = 1 << 19;
constexpr size_t kMaxObjectLines = 4000;
constexpr size_t kMaxLooseSlots = 4000;
constexpr size_t kMaxLooseLines = 600;
constexpr uintptr_t kOwnerSearchBytes = 0x800;

struct PointerHit {
    uintptr_t location;
    uint32_t target;
};

class Report {
public:
    void Line(const char* format, ...) {
        char text[2048];
        va_list args;
        va_start(args, format);
        int length = vsnprintf(text, sizeof text, format, args);
        va_end(args);
        if (length > 0) {
            text_.append(text, std::min<size_t>(static_cast<size_t>(length), sizeof text - 1));
            text_ += "\r\n";
        }
    }

    const std::string& Text() const { return text_; }

    void AppendTo(const std::wstring& path) const {
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
        DWORD written = 0;
        WriteFile(file, text_.data(), static_cast<DWORD>(text_.size()), &written, nullptr);
        CloseHandle(file);
    }

private:
    std::string text_;
};

// Finds every slot in [begin, end) that holds exactly one of the sorted `targets`.
size_t ScanBlock(const uintptr_t* begin, const uintptr_t* end, const uintptr_t* targets, size_t count, PointerHit* out,
                 size_t capacity) {
    size_t found = 0;
    uintptr_t low = targets[0];
    uintptr_t span = targets[count - 1] - low;
    __try {
        for (const uintptr_t* slot = begin; slot < end; ++slot) {
            uintptr_t value = *slot;
            if (value - low > span) {
                continue;
            }
            size_t first = 0;
            size_t last = count;
            while (first < last) {
                size_t middle = (first + last) / 2;
                if (targets[middle] < value) {
                    first = middle + 1;
                } else {
                    last = middle;
                }
            }
            if (first < count && targets[first] == value && found < capacity) {
                out[found].location = reinterpret_cast<uintptr_t>(slot);
                out[found].target = static_cast<uint32_t>(first);
                ++found;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return found;
}

// Appends the committed, writable parts of the allocation that starts at `allocationBase`.
// Returns the address just past the allocation.
uintptr_t AppendAllocation(uintptr_t allocationBase, ArenaVector<Region>& regions) {
    constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    uintptr_t address = allocationBase;
    for (;;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof info) ||
            reinterpret_cast<uintptr_t>(info.AllocationBase) != allocationBase) {
            return address;
        }
        uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (info.State == MEM_COMMIT && (info.Protect & kWritable) && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            regions.push_back({address, end});
        }
        address = end;
    }
}

// The memory worth searching for references: every allocation that holds at least one engine
// object, which is where the engine's heap lives, plus the game's own globals. Memory that cannot
// be tied to the engine this way is deliberately left alone.
ArenaVector<Region> EngineMemory(const ArenaVector<ObjectInfo>& objects) {
    ArenaVector<Region> regions;
    AppendAllocation(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), regions);
    uintptr_t coveredEnd = 0;
    for (const ObjectInfo& object : objects) {
        if (object.address < coveredEnd) {
            continue;
        }
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(object.address), &info, sizeof info) || !info.AllocationBase ||
            IsArenaAllocation(info.AllocationBase)) {
            continue;
        }
        coveredEnd = AppendAllocation(reinterpret_cast<uintptr_t>(info.AllocationBase), regions);
    }
    return regions;
}

uint64_t Scan(const ArenaVector<Region>& regions, const ArenaVector<uintptr_t>& targets, ArenaVector<PointerHit>& hits) {
    constexpr uintptr_t kBlock = 0x10000;
    uint64_t scanned = 0;
    for (const Region& region : regions) {
        for (uintptr_t block = region.begin; block < region.end; block += kBlock) {
            PointerHit found[512];
            size_t count = ScanBlock(reinterpret_cast<const uintptr_t*>(block),
                                     reinterpret_cast<const uintptr_t*>(std::min(region.end, block + kBlock)), targets.data(),
                                     targets.size(), found, std::size(found));
            for (size_t i = 0; i < count && hits.size() < kMaxHits; ++i) {
                hits.push_back(found[i]);
            }
        }
        scanned += region.end - region.begin;
    }
    return scanned;
}

std::string HexAt(uintptr_t address, size_t size) {
    uint8_t bytes[64];
    size = std::min(size, sizeof bytes);
    return SafeRead(address, bytes, size) ? Hex(bytes, size) : "(unreadable)";
}

}  // namespace

std::vector<uint64_t> ReadContainerPackageIds(const std::wstring& utocPath) {
    std::vector<uint64_t> ids;
    HANDLE file = CreateFileW(utocPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return ids;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::vector<uint8_t> data(static_cast<size_t>(std::min<LONGLONG>(size.QuadPart, 256 << 20)));
    DWORD read = 0;
    bool ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) && read == data.size();
    CloseHandle(file);

    // FIoStoreTocHeader: magic, version, ..., TocHeaderSize at 20, TocEntryCount at 24. The FIoChunkIds follow
    // the header: 8 bytes of id, 2 of index, 1 unused, 1 of type.
    uint32_t headerSize = 0;
    uint32_t entryCount = 0;
    if (!ok || data.size() < 28 || memcmp(data.data(), "-==--==--==--==-", 16) != 0) {
        return ids;
    }
    memcpy(&headerSize, &data[20], sizeof headerSize);
    memcpy(&entryCount, &data[24], sizeof entryCount);
    if (headerSize > data.size() || entryCount > (data.size() - headerSize) / 12) {
        return ids;
    }
    for (uint32_t i = 0; i < entryCount; ++i) {
        const uint8_t* chunk = &data[headerSize + static_cast<size_t>(i) * 12];
        if (chunk[11] == kChunkTypeExportBundleData) {
            uint64_t id;
            memcpy(&id, chunk, sizeof id);
            ids.push_back(id);
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

uint64_t PackageIdFromName(const std::string& name) {
    int length = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(), length);
    for (wchar_t& c : wide) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c + (L'a' - L'A'));
        }
    }
    return CityHash64(reinterpret_cast<const char*>(wide.data()), wide.size() * sizeof(wchar_t));
}

Superseded FindSuperseded(const ObjectView& view, const std::vector<uint64_t>& packageIds, bool findSlots) {
    Superseded result;
    const ArenaVector<ObjectInfo>& objects = view.All();
    std::vector<uint8_t> superseded(objects.size(), 0);
    for (size_t i = 0; i < objects.size(); ++i) {
        int32_t index = static_cast<int32_t>(i);
        if (objects[i].outer == 0 && std::binary_search(packageIds.begin(), packageIds.end(), PackageIdFromName(view.Name(index)))) {
            superseded[i] = 1;
            result.packages.push_back(index);
        }
    }
    std::vector<uint8_t> isTarget(objects.size(), 0);
    ArenaVector<uintptr_t> targets;
    for (size_t i = 0; i < objects.size() && !result.packages.empty(); ++i) {
        int32_t package = view.Outermost(static_cast<int32_t>(i));
        if (package >= 0 && superseded[package]) {
            isTarget[i] = 1;
            result.objects.push_back(static_cast<int32_t>(i));
            targets.push_back(objects[i].address);
        }
    }
    if (findSlots && !targets.empty()) {
        ArenaVector<PointerHit> hits;
        Scan(EngineMemory(objects), targets, hits);
        for (const PointerHit& hit : hits) {
            int32_t owner = view.Containing(hit.location);
            if (owner < 0 || isTarget[owner]) {
                continue;
            }
            // Of an object's header only the class (at 0x10) is a reference to an object elsewhere. The
            // flags, index and name before 0x28 are plain numbers that can happen to equal an address.
            size_t offset = hit.location - objects[owner].address;
            if (offset == 0x10 || offset >= 0x28) {
                result.slots.push_back({hit.location, owner, result.objects[hit.target]});
            }
        }
    }
    return result;
}

void ReportReferences(const ObjectSystem& system, uintptr_t target, std::string& out) {
    ArenaVector<ObjectInfo> objects = system.Snapshot();
    ArenaVector<uintptr_t> targets;
    targets.push_back(target);
    ArenaVector<PointerHit> hits;
    Scan(EngineMemory(objects), targets, hits);
    ArenaVector<Region> table = system.TableRanges();

    auto indexOf = [&](uintptr_t address) -> int32_t {
        auto it = std::lower_bound(objects.begin(), objects.end(), address,
                                   [](const ObjectInfo& object, uintptr_t value) { return object.address < value; });
        return it != objects.end() && it->address == address ? static_cast<int32_t>(it - objects.begin()) : -1;
    };
    auto nameOf = [&](int32_t index) { return index < 0 ? std::string("?") : system.Name(objects[index].nameId, objects[index].nameNumber); };
    Report report;
    for (const PointerHit& hit : hits) {
        bool inTable = false;
        for (const Region& range : table) {
            inTable = inTable || (hit.location >= range.begin && hit.location < range.end);
        }
        auto it = std::upper_bound(objects.begin(), objects.end(), hit.location,
                                   [](uintptr_t value, const ObjectInfo& object) { return value < object.address; });
        int32_t owner = -1;
        if (it != objects.begin() && hit.location < (it - 1)->address + (it - 1)->size) {
            owner = static_cast<int32_t>(it - 1 - objects.begin());
        }
        if (inTable) {
            report.Line("%#zx  对象表", hit.location);
        } else if (owner >= 0) {
            std::string path;
            for (int32_t index = owner, depth = 0; index >= 0 && depth < 32; ++depth) {
                path = path.empty() ? nameOf(index) : nameOf(index) + "." + path;
                index = objects[index].outer ? indexOf(objects[index].outer) : -1;
            }
            report.Line("%#zx  %s %s +%#zx", hit.location, nameOf(indexOf(objects[owner].klass)).c_str(), path.c_str(),
                        hit.location - objects[owner].address);
        } else {
            report.Line("%#zx  对象外  前 %s  后 %s", hit.location, HexAt(hit.location - 0x20, 0x20).c_str(),
                        HexAt(hit.location, 0x20).c_str());
        }
    }
    report.Line("共 %zu 处", hits.size());
    out += report.Text();
}

void ProbeSupersededPackages(const ObjectSystem& system, const std::vector<uint64_t>& packageIds,
                             const std::wstring& reportPath, const char* label) {
    ULONGLONG started = GetTickCount64();
    Report report;
    SYSTEMTIME now;
    GetLocalTime(&now);
    report.Line("==================== 探测 %s  %02u:%02u:%02u ====================", label, now.wHour, now.wMinute, now.wSecond);

    ArenaVector<ObjectInfo> objects = system.Snapshot();
    auto indexOf = [&](uintptr_t address) -> int32_t {
        auto it = std::lower_bound(objects.begin(), objects.end(), address,
                                   [](const ObjectInfo& object, uintptr_t value) { return object.address < value; });
        return it != objects.end() && it->address == address ? static_cast<int32_t>(it - objects.begin()) : -1;
    };
    auto containing = [&](uintptr_t location) -> int32_t {
        auto it = std::upper_bound(objects.begin(), objects.end(), location,
                                   [](uintptr_t value, const ObjectInfo& object) { return value < object.address; });
        if (it == objects.begin()) {
            return -1;
        }
        --it;
        return location < it->address + it->size ? static_cast<int32_t>(it - objects.begin()) : -1;
    };
    auto nameOf = [&](int32_t index) { return index < 0 ? std::string("?") : system.Name(objects[index].nameId, objects[index].nameNumber); };
    auto classOf = [&](int32_t index) { return index < 0 ? std::string("?") : nameOf(indexOf(objects[index].klass)); };
    auto pathOf = [&](int32_t index) {
        std::string path;
        for (int depth = 0; index >= 0 && depth < 32; ++depth) {
            path = path.empty() ? nameOf(index) : nameOf(index) + "." + path;
            index = objects[index].outer ? indexOf(objects[index].outer) : -1;
        }
        return path;
    };

    // Packages the container supersedes, then every object that lives in one of them.
    std::vector<uint8_t> superseded(objects.size(), 0);
    size_t packageCount = 0;
    size_t supersededCount = 0;
    for (size_t i = 0; i < objects.size(); ++i) {
        if (objects[i].outer != 0) {
            continue;
        }
        ++packageCount;
        if (std::binary_search(packageIds.begin(), packageIds.end(), PackageIdFromName(nameOf(static_cast<int32_t>(i))))) {
            superseded[i] = 1;
            ++supersededCount;
        }
    }
    std::vector<uint8_t> isTarget(objects.size(), 0);
    ArenaVector<uintptr_t> targets;
    std::vector<int32_t> targetIndex;
    for (size_t i = 0; i < objects.size(); ++i) {
        int32_t current = static_cast<int32_t>(i);
        for (int depth = 0; current >= 0 && objects[current].outer != 0 && depth < 64; ++depth) {
            current = indexOf(objects[current].outer);
        }
        if (current >= 0 && superseded[current]) {
            isTarget[i] = 1;
            targets.push_back(objects[i].address);
            targetIndex.push_back(static_cast<int32_t>(i));
        }
    }

    report.Line("对象 %zu 个，包 %zu 个；新包里有 %zu 个包，其中 %zu 个已加载，属于它们的对象 %zu 个", objects.size(), packageCount,
                packageIds.size(), supersededCount, targets.size());
    report.Line("");
    report.Line("---- 已加载的旧对象（T<编号> 类 路径 对象标志 内部标志）----");
    for (size_t t = 0; t < targets.size() && t < kMaxObjectLines; ++t) {
        const ObjectInfo& object = objects[targetIndex[t]];
        report.Line("T%zu %s %s %#x %#x%s", t, classOf(targetIndex[t]).c_str(), pathOf(targetIndex[t]).c_str(), object.objectFlags,
                    object.internalFlags, (object.internalFlags & kInternalRootSet) ? " [RootSet]" : "");
    }

    size_t tableHits = 0;
    size_t internalHits = 0;
    size_t objectHits = 0;
    size_t looseHits = 0;
    size_t ownerHits = 0;
    uint64_t scannedBytes = 0;
    if (!targets.empty()) {
        ArenaVector<Region> memory = EngineMemory(objects);
        ArenaVector<Region> table = system.TableRanges();
        uintptr_t mainImage = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto inTable = [&](uintptr_t location) {
            for (const Region& range : table) {
                if (location >= range.begin && location < range.end) {
                    return true;
                }
            }
            return false;
        };
        auto inMainImage = [&](uintptr_t location) {
            MEMORY_BASIC_INFORMATION info{};
            return VirtualQuery(reinterpret_cast<const void*>(location), &info, sizeof info) &&
                   reinterpret_cast<uintptr_t>(info.AllocationBase) == mainImage;
        };
        auto describe = [&](uintptr_t location) {
            char text[64];
            if (inMainImage(location)) {
                snprintf(text, sizeof text, "主模块全局 RVA %#zx", location - mainImage);
            } else {
                snprintf(text, sizeof text, "堆 %#zx", location);
            }
            return std::string(text);
        };

        ArenaVector<PointerHit> hits;
        hits.reserve(1 << 16);
        scannedBytes = Scan(memory, targets, hits);

        // Pass 1: slots that point straight at an old object.
        report.Line("");
        report.Line("---- 直接指向旧对象的其他对象（类 路径 +偏移 -> T<编号>）----");
        std::map<std::string, size_t> referencerClasses;
        ArenaVector<PointerHit> loose;  // slots outside any object, usually TArray/TMap/TSet storage
        for (const PointerHit& hit : hits) {
            if (inTable(hit.location)) {
                ++tableHits;
                continue;
            }
            int32_t owner = containing(hit.location);
            if (owner >= 0 && isTarget[owner]) {
                ++internalHits;
            } else if (owner >= 0) {
                std::string ownerClass = classOf(owner);
                ++referencerClasses[ownerClass];
                if (objectHits++ < kMaxObjectLines) {
                    report.Line("%s %s +%#zx -> T%u%s", ownerClass.c_str(), pathOf(owner).c_str(),
                                hit.location - objects[owner].address, hit.target,
                                (objects[owner].internalFlags & kInternalRootSet) ? " [引用者是 RootSet]" : "");
                }
            } else {
                ++looseHits;
                if (loose.size() < kMaxLooseSlots) {
                    loose.push_back(hit);
                }
            }
        }
        std::sort(loose.begin(), loose.end(), [](const PointerHit& a, const PointerHit& b) { return a.location < b.location; });
        report.Line("");
        report.Line("---- 直接引用者按类汇总 ----");
        for (const auto& [name, count] : referencerClasses) {
            report.Line("%6zu  %s", count, name.c_str());
        }

        report.Line("");
        report.Line("---- 不在任何对象里的指针（位置 -> T<编号>，附前后内存）----");
        for (size_t i = 0; i < loose.size() && i < kMaxLooseLines; ++i) {
            report.Line("%s -> T%u", describe(loose[i].location).c_str(), loose[i].target);
            report.Line("    前 %s", HexAt(loose[i].location - 0x20, 0x20).c_str());
            report.Line("    后 %s", HexAt(loose[i].location, 0x30).c_str());
        }

        // Pass 2: such a slot belongs to some heap block. Whoever points at an address shortly before it
        // is a candidate owner of that block. The block cannot start before an object that lies in between.
        report.Line("");
        report.Line("---- 可能持有上述堆内存的对象（类 路径 +偏移 -> 堆块，块内 +距离 处指向 T<编号>）----");
        if (!loose.empty()) {
            ArenaVector<uintptr_t> starts;
            starts.reserve(loose.size() * (kOwnerSearchBytes / sizeof(uintptr_t)));
            for (const PointerHit& slot : loose) {
                for (uintptr_t back = 0; back < kOwnerSearchBytes && containing(slot.location - back) < 0; back += sizeof(uintptr_t)) {
                    starts.push_back(slot.location - back);
                }
            }
            std::sort(starts.begin(), starts.end());
            starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

            ArenaVector<PointerHit> ownerSlots;
            ownerSlots.reserve(1 << 16);
            scannedBytes += Scan(memory, starts, ownerSlots);
            std::map<std::string, size_t> ownerClasses;
            for (const PointerHit& hit : ownerSlots) {
                if (inTable(hit.location)) {
                    continue;
                }
                int32_t owner = containing(hit.location);
                if (owner >= 0 ? isTarget[owner] != 0 : !inMainImage(hit.location)) {
                    continue;
                }
                uintptr_t block = starts[hit.target];
                auto first = std::lower_bound(loose.begin(), loose.end(), block,
                                              [](const PointerHit& slot, uintptr_t value) { return slot.location < value; });
                if (first == loose.end() || first->location - block >= kOwnerSearchBytes) {
                    continue;
                }
                std::string ownerClass = owner >= 0 ? classOf(owner) : "(主模块全局)";
                ++ownerClasses[ownerClass];
                if (ownerHits++ < kMaxObjectLines) {
                    std::string where = owner >= 0 ? pathOf(owner) : describe(hit.location);
                    report.Line("%s %s +%#zx -> 堆块 %#zx，块内 +%#zx 处指向 T%u%s", ownerClass.c_str(), where.c_str(),
                                owner >= 0 ? hit.location - objects[owner].address : size_t{0}, block, first->location - block,
                                first->target,
                                (owner >= 0 && (objects[owner].internalFlags & kInternalRootSet)) ? " [RootSet]" : "");
                }
            }
            report.Line("");
            report.Line("---- 可能的持有者按类汇总 ----");
            for (const auto& [name, count] : ownerClasses) {
                report.Line("%6zu  %s", count, name.c_str());
            }
        }
    }

    report.Line("");
    report.Line("汇总: 扫描 %.1f GB；对象表自身 %zu 处，旧对象之间 %zu 处，其他对象直接引用 %zu 处，对象外的指针 %zu 处，可能的持有者 %zu 处；耗时 %.1f 秒",
                scannedBytes / 1073741824.0, tableHits, internalHits, objectHits, looseHits, ownerHits,
                (GetTickCount64() - started) / 1000.0);
    report.Line("");
    report.AppendTo(reportPath);

    Log("探测 %s: 新包覆盖了 %zu 个已加载的包、%zu 个对象；其他对象直接引用 %zu 处，对象外的指针 %zu 处；耗时 %.1f 秒", label,
        supersededCount, targets.size(), objectHits, looseHits, (GetTickCount64() - started) / 1000.0);
}
