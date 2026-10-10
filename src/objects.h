#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "arena.h"
#include "image.h"

struct ObjectInfo {
    uintptr_t address = 0;
    uintptr_t klass = 0;
    uintptr_t outer = 0;
    uint32_t nameId = 0;
    uint32_t nameNumber = 0;
    uint32_t objectFlags = 0;    // EObjectFlags
    uint32_t internalFlags = 0;  // EInternalObjectFlags
    uint32_t size = 0;           // UClass::PropertiesSize of its class, 0 when unknown
};

// Read-only view of the engine's object table (GUObjectArray) and name pool. Both are found by
// their shape in the game's writable data, so nothing here depends on addresses or signatures.
class ObjectSystem {
public:
    // The object table only exists once the engine has initialised its object system, which is well
    // after pak mounting comes up, so this may have to be retried.
    bool Locate(const Image& image);
    bool Ready() const { return objectArray_ != 0 && nameBlocks_ != 0; }

    // Class, name and raw header of the object at `address`. Needs only the name pool.
    std::string Describe(uintptr_t address) const;

    // Every live object, sorted by address.
    ArenaVector<ObjectInfo> Snapshot() const;
    std::string Name(uint32_t id, uint32_t number) const;

    // The chunks of the object table, which hold a pointer to every object.
    ArenaVector<Region> TableRanges() const;

    // Names in the pool containing `text` (ASCII, case-insensitive), as (id, name) pairs.
    std::vector<std::pair<uint32_t, std::string>> FindNames(const std::string& text, size_t limit) const;

private:
    bool FindNamePool(const Image& image);
    bool FindObjectArray(const Image& image);
    void FindPropertiesSizeOffset();
    template <class Visitor>
    void ForEachObject(Visitor&& visit) const;

    uintptr_t nameBlocks_ = 0;    // FNameEntryAllocator::Blocks
    uintptr_t objectArray_ = 0;   // FChunkedFixedUObjectArray
    size_t itemSize_ = 0;         // sizeof(FUObjectItem)
    bool itemPacked_ = false;     // UE_PACK_FUOBJECT_ITEM
    size_t propertiesSizeOffset_ = 0;
};
