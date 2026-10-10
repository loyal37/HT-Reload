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
    // Whether a name starts at `id` in the pool. Name ids reach into the range of low heap addresses,
    // so a stored value for which this holds may be a name rather than a pointer.
    bool IsNameId(uint32_t id) const;

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

// EObjectFlags used here.
constexpr uint32_t kObjectPublic = 0x01;
constexpr uint32_t kObjectClassDefault = 0x10;
constexpr uint32_t kObjectArchetype = 0x20;

// One snapshot of the object table with lookups by address.
class ObjectView {
public:
    explicit ObjectView(const ObjectSystem& system) : system_(system), objects_(system.Snapshot()) {}

    const ArenaVector<ObjectInfo>& All() const { return objects_; }
    const ObjectInfo& operator[](int32_t index) const { return objects_[index]; }

    // Index of the object at `address`, or -1.
    int32_t IndexOf(uintptr_t address) const;
    // Index of the object whose body contains `address`, or -1.
    int32_t Containing(uintptr_t address) const;
    // The package an object lives in (itself, for a package).
    int32_t Outermost(int32_t index) const;

    std::string Name(int32_t index) const;
    std::string ClassName(int32_t index) const;
    std::string Path(int32_t index) const;
    // Whether the object's class is `className` or derives from it.
    bool IsA(int32_t index, const char* className) const;

private:
    const ObjectSystem& system_;
    ArenaVector<ObjectInfo> objects_;
};
