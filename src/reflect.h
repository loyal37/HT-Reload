#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "objects.h"

// An array property whose elements hold object references: a TArray of object pointers, or of
// structs with object pointers in them.
struct ArrayPlan {
    uint32_t offset = 0;           // of the TArray inside its owner
    uint32_t elementSize = 0;
    std::vector<uint32_t> fields;  // offsets of the object pointers inside one element
    std::string name;
};

// A reference to one of a set of objects, held in the heap storage of an array property of another object.
struct ArrayReference {
    int32_t owner = -1;   // index into the view
    int32_t target = -1;  // index into the view
    uint32_t arrayOffset = 0;
    uint32_t elementSize = 0;
    uint32_t fieldOffset = 0;
    uint32_t index = 0;   // of the element at the time of the walk
    std::string property;
};

// Reads the engine's reflection data (UStruct, FField, FProperty) of the running game.
// The members of FProperty's subclasses are found by their content, not by fixed offsets.
class Reflection {
public:
    Reflection(const ObjectSystem& system, const ObjectView& view);

    // False when the layout of array and struct properties could not be worked out.
    bool Ready() const { return innerOffset_ != 0 && structOffset_ != 0; }

    // The class, script struct or function of that name, 0 if there is none.
    uintptr_t FindStruct(const char* name, const char* kind) const;
    // Offset of the property `propertyName` declared by the struct at `owner`, or -1.
    int32_t PropertyOffset(uintptr_t owner, const char* propertyName) const;
    // UStruct::PropertiesSize.
    uint32_t SizeOf(uintptr_t owner) const;

    // The arrays with object references that the class at `klass` and its parents declare.
    const std::vector<ArrayPlan>& ArraysOf(uintptr_t klass);

    // Every reference to an object in `targets` (indices into the view, ascending) that sits in the
    // array storage of an object outside that set.
    std::vector<ArrayReference> FindArrayReferences(const std::vector<int32_t>& targets);

private:
    struct Field {
        std::string type;
        std::string name;
        uintptr_t next = 0;
        int32_t arrayDim = 0;
        int32_t elementSize = 0;
        int32_t offset = 0;
    };
    bool ReadField(uintptr_t address, Field& field) const;
    uintptr_t FindProperty(uintptr_t owner, const char* propertyName) const;
    void CollectObjectFields(uintptr_t owner, uint32_t base, int depth, std::vector<uint32_t>& fields) const;

    const ObjectSystem& system_;
    const ObjectView& view_;
    size_t innerOffset_ = 0;   // FArrayProperty::Inner
    size_t structOffset_ = 0;  // FStructProperty::Struct
    std::unordered_map<uintptr_t, std::vector<ArrayPlan>> arrays_;
};

// The header of a TArray.
struct ArrayHeader {
    uintptr_t data = 0;
    int32_t count = 0;
    int32_t capacity = 0;
};
bool ReadArrayHeader(uintptr_t address, ArrayHeader& header);
