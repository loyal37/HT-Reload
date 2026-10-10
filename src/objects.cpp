#include "objects.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "log.h"

namespace {

constexpr size_t kPage = 0x1000;
constexpr int32_t kItemsPerChunk = 64 * 1024;
constexpr uint32_t kNameBlockOffsetBits = 16;
constexpr uint32_t kNameMaxBlocks = 1 << 13;
constexpr size_t kNameStride = 2;
constexpr size_t kMaxItemSize = 0x28;

// UObjectBase
struct ObjectHeader {
    uintptr_t vtable;
    uint32_t objectFlags;
    int32_t internalIndex;
    uintptr_t klass;
    uint32_t nameId;
    uint32_t nameNumber;
    uintptr_t outer;
};

// FChunkedFixedUObjectArray
struct ObjectArrayHeader {
    uintptr_t chunks;
    uintptr_t preAllocated;
    int32_t maxElements;
    int32_t numElements;
    int32_t maxChunks;
    int32_t numChunks;
};

// With UE_PACK_FUOBJECT_ITEM the pointer's low 32 bits (shifted right by 3) come first and its
// high 13 bits share a word with the flags, which start at bit 14.
constexpr uint32_t kPackedPointerMask = 0x3FFF;

uintptr_t ItemObject(const uint8_t* item, bool packed) {
    if (!packed) {
        uintptr_t object;
        memcpy(&object, item, sizeof object);
        return object;
    }
    uint32_t low;
    uint32_t flags;
    memcpy(&low, item, sizeof low);
    memcpy(&flags, item + 4, sizeof flags);
    return (static_cast<uintptr_t>(flags & kPackedPointerMask) << 35) | (static_cast<uintptr_t>(low) << 3);
}

uint32_t ItemFlags(const uint8_t* item, bool packed) {
    uint32_t flags;
    memcpy(&flags, item + (packed ? 4 : 8), sizeof flags);
    return packed ? flags & ~kPackedPointerMask : flags;
}

// FNameEntryHeader without case-preserving names: bIsWide:1, LowercaseProbeHash:5, Len:10.
bool IsNameEntry(const uint8_t* entry, const char* text) {
    uint16_t header;
    memcpy(&header, entry, sizeof header);
    size_t length = strlen(text);
    return !(header & 1) && (header >> 6) == length && memcmp(entry + 2, text, length) == 0;
}

template <class Visitor>
void ForEachWritableSlot(const Image& image, Visitor&& visit) {
    for (const Region& region : image.writable) {
        for (uintptr_t page = region.begin; page < region.end; page += kPage) {
            uintptr_t slots[kPage / sizeof(uintptr_t)];
            size_t count = std::min<size_t>(kPage, region.end - page) / sizeof(uintptr_t);
            if (!SafeRead(page, slots, count * sizeof(uintptr_t))) {
                continue;
            }
            for (size_t i = 0; i < count; ++i) {
                if (visit(page + i * sizeof(uintptr_t), slots[i])) {
                    return;
                }
            }
        }
    }
}

}  // namespace

bool ObjectSystem::Locate(const Image& image) {
    if (!nameBlocks_ && !FindNamePool(image)) {
        Log("对象系统: 没找到名字池（开头不是 None / ByteProperty，可能被游戏改过）");
        return false;
    }
    if (!objectArray_ && !FindObjectArray(image)) {
        Log("对象系统: 没找到对象表");
        return false;
    }
    if (!propertiesSizeOffset_) {
        FindPropertiesSizeOffset();
    }
    return true;
}

std::string ObjectSystem::Describe(uintptr_t object) const {
    ObjectHeader header;
    ObjectHeader classHeader;
    if (!nameBlocks_ || !object || !SafeRead(object, &header, sizeof header)) {
        return "(读不到)";
    }
    std::string className = SafeRead(header.klass, &classHeader, sizeof classHeader) ? Name(classHeader.nameId, classHeader.nameNumber) : "?";
    char text[160];
    snprintf(text, sizeof text, "%s %s，序号 %d，标志 %#x", className.c_str(), Name(header.nameId, header.nameNumber).c_str(),
             header.internalIndex, header.objectFlags);
    uint8_t raw[sizeof header];
    memcpy(raw, &header, sizeof raw);
    return std::string(text) + "，原始 " + Hex(raw, sizeof raw);
}

// FNameEntryAllocator { FRWLock Lock; uint32 CurrentBlock; uint32 CurrentByteCursor; uint8* Blocks[]; }
// lives in the image and its first block always starts with the entries "None" and "ByteProperty".
bool ObjectSystem::FindNamePool(const Image& image) {
    ReadableCache cache;
    ForEachWritableSlot(image, [&](uintptr_t slot, uintptr_t value) {
        uint8_t head[24];
        if (!PlausibleHeapPointer(image, value) || !cache.Readable(value, sizeof head) ||
            !SafeRead(value, head, sizeof head) || !IsNameEntry(head, "None") || !IsNameEntry(head + 6, "ByteProperty")) {
            return false;
        }
        uint32_t cursor[2] = {};
        SafeRead(slot - sizeof cursor, cursor, sizeof cursor);
        if (cursor[0] >= kNameMaxBlocks) {
            return false;
        }
        nameBlocks_ = slot;
        Log("对象系统: 名字池 Blocks RVA %#zx，当前块 %u，块内游标 %#x", slot - image.base, cursor[0], cursor[1]);
        return true;
    });
    return nameBlocks_ != 0;
}

// FChunkedFixedUObjectArray inside GUObjectArray. PreAllocate() sets MaxChunks = Max / 65536 + 1 and
// MaxElements = MaxChunks * 65536, which makes the counters recognisable on their own. The item
// layout is then confirmed by the first items pointing at objects whose InternalIndex equals
// their position.
bool ObjectSystem::FindObjectArray(const Image& image) {
    struct Layout {
        size_t size;
        bool packed;
    };
    static const Layout layouts[] = {{0x18, false}, {0x10, true}, {0x20, false}, {kMaxItemSize, false}};
    constexpr int kProbed = 64;

    ReadableCache cache;
    int candidates = 0;
    ForEachWritableSlot(image, [&](uintptr_t slot, uintptr_t value) {
        ObjectArrayHeader header;
        if (!PlausibleHeapPointer(image, value) || !SafeRead(slot, &header, sizeof header) || header.maxChunks < 1 ||
            header.maxChunks > 0x7FFF || header.maxElements != header.maxChunks * kItemsPerChunk || header.numElements < 1 ||
            header.numElements > header.maxElements || header.numChunks < 1 || header.numChunks > header.maxChunks) {
            return false;
        }
        uintptr_t firstChunk = 0;
        bool chunkReadable = cache.Readable(header.chunks, sizeof firstChunk) &&
                             SafeRead(header.chunks, &firstChunk, sizeof firstChunk) && PlausibleHeapPointer(image, firstChunk);
        int bestMatching = 0;
        for (const Layout& layout : layouts) {
            uint8_t items[kProbed * kMaxItemSize];
            if (!chunkReadable || !cache.Readable(firstChunk, kProbed * layout.size) ||
                !SafeRead(firstChunk, items, kProbed * layout.size)) {
                continue;
            }
            int matching = 0;
            for (int i = 0; i < kProbed; ++i) {
                uintptr_t object = ItemObject(items + i * layout.size, layout.packed);
                ObjectHeader objectHeader;
                if (PlausibleHeapPointer(image, object) && cache.Readable(object, sizeof objectHeader) &&
                    SafeRead(object, &objectHeader, sizeof objectHeader) && objectHeader.internalIndex == i &&
                    image.Contains(objectHeader.vtable)) {
                    ++matching;
                }
            }
            bestMatching = std::max(bestMatching, matching);
            if (matching >= kProbed * 3 / 4) {
                objectArray_ = slot;
                itemSize_ = layout.size;
                itemPacked_ = layout.packed;
                Log("对象系统: 对象表 RVA %#zx，%d 个对象（上限 %d），%d/%d 块，条目 %#zx 字节%s", slot - image.base,
                    header.numElements, header.maxElements, header.numChunks, header.maxChunks, layout.size,
                    layout.packed ? "（压缩指针）" : "");
                return true;
            }
        }

        // The counters fit but the items do not: record what is there so the layout can be worked out.
        if (candidates++ < 4) {
            Log("对象系统: 候选 RVA %#zx 计数吻合但条目不符（最多 %d/%d 个对上）：上限 %d，当前 %d，块 %d/%d，块表 %#zx，预分配 %#zx",
                slot - image.base, bestMatching, kProbed, header.maxElements, header.numElements, header.numChunks,
                header.maxChunks, header.chunks, header.preAllocated);
            uint8_t raw[0x60];
            if (chunkReadable && cache.Readable(firstChunk, sizeof raw) && SafeRead(firstChunk, raw, sizeof raw)) {
                Log("  首块 %#zx: %s", firstChunk, Hex(raw, sizeof raw).c_str());
                for (size_t offset : {size_t{0}, size_t{0x18}, size_t{0x30}}) {
                    uintptr_t object;
                    memcpy(&object, raw + offset, sizeof object);
                    uint8_t body[0x40];
                    if (PlausibleHeapPointer(image, object) && cache.Readable(object, sizeof body) &&
                        SafeRead(object, body, sizeof body)) {
                        Log("  条目 +%#zx -> %#zx: %s", offset, object, Hex(body, sizeof body).c_str());
                    }
                }
            }
        }
        return false;
    });
    if (objectArray_ || candidates > 0) {
        return objectArray_ != 0;
    }

    // No counters of the stock shape. Look for the table by its contents alone and show what surrounds it.
    Log("对象系统: 可写数据里没有形似对象表计数的位置，改为按内容查找");
    ForEachWritableSlot(image, [&](uintptr_t slot, uintptr_t value) {
        uintptr_t firstChunk = 0;
        if (!PlausibleHeapPointer(image, value) || !cache.Readable(value, sizeof firstChunk) ||
            !SafeRead(value, &firstChunk, sizeof firstChunk) || !PlausibleHeapPointer(image, firstChunk)) {
            return false;
        }
        for (const Layout& layout : layouts) {
            uint8_t items[kProbed * kMaxItemSize];
            if (!cache.Readable(firstChunk, kProbed * layout.size) || !SafeRead(firstChunk, items, kProbed * layout.size)) {
                continue;
            }
            int matching = 0;
            for (int i = 0; i < kProbed && matching + 4 >= i; ++i) {
                uintptr_t object = ItemObject(items + i * layout.size, layout.packed);
                ObjectHeader objectHeader;
                if (PlausibleHeapPointer(image, object) && cache.Readable(object, sizeof objectHeader) &&
                    SafeRead(object, &objectHeader, sizeof objectHeader) && objectHeader.internalIndex == i &&
                    image.Contains(objectHeader.vtable)) {
                    ++matching;
                }
            }
            if (matching < kProbed * 3 / 4) {
                continue;
            }
            uint8_t around[0x50] = {};
            SafeRead(slot - 0x20, around, sizeof around);
            Log("对象系统: RVA %#zx 的内容是对象表（条目 %#zx 字节%s），但计数字段不是原版布局。前后内存: %s", slot - image.base,
                layout.size, layout.packed ? "，压缩指针" : "", Hex(around, sizeof around).c_str());
            return true;
        }
        return false;
    });
    return false;
}

template <class Visitor>
void ObjectSystem::ForEachObject(Visitor&& visit) const {
    ObjectArrayHeader header;
    if (!objectArray_ || !SafeRead(objectArray_, &header, sizeof header) || header.numElements <= 0 ||
        header.numElements > (1 << 26)) {
        return;
    }
    ArenaVector<uint8_t> items(static_cast<size_t>(kItemsPerChunk) * itemSize_);
    ReadableCache cache;
    for (int32_t first = 0; first < header.numElements; first += kItemsPerChunk) {
        uintptr_t chunk = 0;
        int32_t count = std::min(kItemsPerChunk, header.numElements - first);
        if (!SafeRead(header.chunks + (first / kItemsPerChunk) * sizeof(uintptr_t), &chunk, sizeof chunk) || !chunk ||
            !SafeRead(chunk, items.data(), count * itemSize_)) {
            continue;
        }
        for (int32_t i = 0; i < count; ++i) {
            const uint8_t* item = items.data() + i * itemSize_;
            uintptr_t object = ItemObject(item, itemPacked_);
            ObjectHeader objectHeader;
            if (!object || (object & 7) || !cache.Readable(object, sizeof objectHeader) ||
                !SafeRead(object, &objectHeader, sizeof objectHeader) || objectHeader.internalIndex != first + i) {
                continue;
            }
            ObjectInfo info;
            info.address = object;
            info.klass = objectHeader.klass;
            info.outer = objectHeader.outer;
            info.nameId = objectHeader.nameId;
            info.nameNumber = objectHeader.nameNumber;
            info.objectFlags = objectHeader.objectFlags;
            info.internalFlags = ItemFlags(item, itemPacked_);
            visit(info);
        }
    }
}

// UClass::PropertiesSize is followed by MinAlignment, which is only 16 bits wide. For the classes "Object"
// and "Field" the pair is (sizeof(UObject), 8) and (sizeof(UObject) + 8, 8), which pins down the offset
// (0x58 in UE 5.6 and in the game).
void ObjectSystem::FindPropertiesSizeOffset() {
    uintptr_t classClass = 0;
    ForEachObject([&](const ObjectInfo& info) {
        if (info.klass == info.address) {
            classClass = info.address;
        }
    });
    uintptr_t objectClass = 0;
    uintptr_t fieldClass = 0;
    ForEachObject([&](const ObjectInfo& info) {
        if (info.klass != classClass || info.nameNumber != 0) {
            return;
        }
        std::string name = Name(info.nameId, 0);
        if (name == "Object") {
            objectClass = info.address;
        } else if (name == "Field") {
            fieldClass = info.address;
        }
    });

    int32_t objectFields[0x40];
    int32_t fieldFields[0x40];
    if (classClass && objectClass && fieldClass && SafeRead(objectClass, objectFields, sizeof objectFields) &&
        SafeRead(fieldClass, fieldFields, sizeof fieldFields)) {
        for (size_t i = sizeof(ObjectHeader) / 4; i + 1 < std::size(objectFields); ++i) {
            if (objectFields[i] == static_cast<int32_t>(sizeof(ObjectHeader)) && (objectFields[i + 1] & 0xFFFF) == 8 &&
                fieldFields[i] == static_cast<int32_t>(sizeof(ObjectHeader)) + 8 && (fieldFields[i + 1] & 0xFFFF) == 8) {
                propertiesSizeOffset_ = i * 4;
                break;
            }
        }
    }
    if (propertiesSizeOffset_) {
        Log("对象系统: UClass 的对象大小字段在偏移 %#zx", propertiesSizeOffset_);
    } else {
        Log("对象系统: 没能确定 UClass 的对象大小字段（Class %#zx，Object %#zx，Field %#zx），对象大小改用相邻对象的间距估计", classClass,
            objectClass, fieldClass);
        for (uintptr_t klass : {objectClass, fieldClass}) {
            uint8_t raw[0x40];
            for (size_t offset = sizeof(ObjectHeader); klass && offset < 0xE8 && SafeRead(klass + offset, raw, sizeof raw); offset += sizeof raw) {
                Log("  类 %#zx +%#zx %s", klass, offset, Hex(raw, sizeof raw).c_str());
            }
        }
    }
}

ArenaVector<ObjectInfo> ObjectSystem::Snapshot() const {
    ArenaVector<ObjectInfo> objects;
    objects.reserve(1 << 20);
    ReadableCache cache;
    ForEachObject([&](ObjectInfo& info) {
        int32_t size = 0;
        if (propertiesSizeOffset_ && cache.Readable(info.klass + propertiesSizeOffset_, sizeof size) &&
            SafeRead(info.klass + propertiesSizeOffset_, &size, sizeof size) && size >= static_cast<int32_t>(sizeof(ObjectHeader)) &&
            size < (1 << 24)) {
            info.size = static_cast<uint32_t>(size);
        }
        objects.push_back(info);
    });
    std::sort(objects.begin(), objects.end(), [](const ObjectInfo& a, const ObjectInfo& b) { return a.address < b.address; });
    // Without a class size, assume an object ends where the next one starts, within reason.
    for (size_t i = 0; i < objects.size(); ++i) {
        if (objects[i].size == 0) {
            uintptr_t gap = i + 1 < objects.size() ? objects[i + 1].address - objects[i].address : 0x100;
            objects[i].size = static_cast<uint32_t>(std::min<uintptr_t>(gap, 0x1000));
        }
    }
    return objects;
}

std::string ObjectSystem::Name(uint32_t id, uint32_t number) const {
    uint32_t blockIndex = id >> kNameBlockOffsetBits;
    uintptr_t block = 0;
    uint16_t header = 0;
    if (blockIndex >= kNameMaxBlocks || !SafeRead(nameBlocks_ + blockIndex * sizeof(uintptr_t), &block, sizeof block) || !block) {
        return "?";
    }
    uintptr_t entry = block + (id & ((1u << kNameBlockOffsetBits) - 1)) * kNameStride;
    if (!SafeRead(entry, &header, sizeof header)) {
        return "?";
    }
    size_t length = header >> 6;
    std::string name;
    if (header & 1) {
        std::wstring wide(length, L'\0');
        if (!SafeRead(entry + 2, wide.data(), length * sizeof(wchar_t))) {
            return "?";
        }
        name = Utf8(wide);
    } else {
        name.resize(length);
        if (!SafeRead(entry + 2, name.data(), length)) {
            return "?";
        }
    }
    if (number) {
        name += "_" + std::to_string(number - 1);
    }
    return name;
}

std::vector<std::pair<uint32_t, std::string>> ObjectSystem::FindNames(const std::string& text, size_t limit) const {
    constexpr size_t kBlockBytes = kNameStride << kNameBlockOffsetBits;
    std::vector<std::pair<uint32_t, std::string>> found;
    std::string wanted = text;
    CharLowerBuffA(wanted.data(), static_cast<DWORD>(wanted.size()));
    uint32_t cursor[2] = {};  // CurrentBlock, CurrentByteCursor
    if (!nameBlocks_ || !SafeRead(nameBlocks_ - sizeof cursor, cursor, sizeof cursor) || cursor[0] >= kNameMaxBlocks) {
        return found;
    }
    std::vector<uint8_t> block(kBlockBytes);
    for (uint32_t index = 0; index <= cursor[0] && found.size() < limit; ++index) {
        uintptr_t address = 0;
        size_t used = index == cursor[0] ? std::min<size_t>(cursor[1], kBlockBytes) : kBlockBytes;
        if (!SafeRead(nameBlocks_ + index * sizeof(uintptr_t), &address, sizeof address) || !address ||
            !SafeRead(address, block.data(), used)) {
            continue;
        }
        for (size_t offset = 0; offset + 2 <= used && found.size() < limit;) {
            uint16_t header;
            memcpy(&header, &block[offset], sizeof header);
            size_t length = header >> 6;
            size_t bytes = (header & 1) ? length * 2 : length;
            if (length == 0 || offset + 2 + bytes > used) {
                break;  // padding at the end of the block
            }
            if (!(header & 1)) {
                std::string name(reinterpret_cast<const char*>(&block[offset + 2]), length);
                std::string lower = name;
                CharLowerBuffA(lower.data(), static_cast<DWORD>(lower.size()));
                if (lower.find(wanted) != std::string::npos) {
                    found.emplace_back((index << kNameBlockOffsetBits) | static_cast<uint32_t>(offset / kNameStride), name);
                }
            }
            offset += (2 + bytes + kNameStride - 1) & ~(kNameStride - 1);
        }
    }
    return found;
}

ArenaVector<Region> ObjectSystem::TableRanges() const {
    ArenaVector<Region> ranges;
    ObjectArrayHeader header;
    if (!objectArray_ || !SafeRead(objectArray_, &header, sizeof header) || header.numChunks <= 0 || header.numChunks > 4096) {
        return ranges;
    }
    for (int32_t i = 0; i < header.numChunks; ++i) {
        uintptr_t chunk = 0;
        if (SafeRead(header.chunks + i * sizeof(uintptr_t), &chunk, sizeof chunk) && chunk) {
            ranges.push_back({chunk, chunk + static_cast<size_t>(kItemsPerChunk) * itemSize_});
        }
    }
    return ranges;
}
