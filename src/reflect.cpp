#include "reflect.h"

#include <algorithm>
#include <cstring>

#include "image.h"

namespace {

// UStruct.
constexpr size_t kSuperStruct = 0x40;
constexpr size_t kChildProperties = 0x50;
constexpr size_t kPropertiesSize = 0x58;
// FField. Its class starts with the class's name.
constexpr size_t kFieldClass = 0x08;
constexpr size_t kFieldNext = 0x18;
constexpr size_t kFieldName = 0x20;
// FProperty.
constexpr size_t kArrayDim = 0x30;
constexpr size_t kElementSize = 0x34;
constexpr size_t kOffsetInternal = 0x44;
constexpr size_t kPropertyBytes = 0x48;
// Where the members that subclasses of FProperty add are looked for.
constexpr size_t kSubclassBegin = 0x48;
constexpr size_t kSubclassEnd = 0xC0;

constexpr int kMaxProperties = 4000;
constexpr int kMaxParents = 64;
constexpr int kMaxStructDepth = 3;
constexpr int32_t kMaxElements = 1 << 22;
constexpr size_t kMaxArrayBytes = 64u << 20;

bool HoldsObject(const std::string& type) {
    return type == "ObjectProperty" || type == "ClassProperty" || type == "ObjectPtrProperty" || type == "ClassPtrProperty";
}

}  // namespace

bool ReadArrayHeader(uintptr_t address, ArrayHeader& header) {
    return SafeRead(address, &header, sizeof header) && header.data != 0 && header.data % sizeof(uintptr_t) == 0 &&
           header.count > 0 && header.count <= header.capacity && header.count <= kMaxElements;
}

Reflection::Reflection(const ObjectSystem& system, const ObjectView& view) : system_(system), view_(view) {
    // The inner property of an array carries the array's own name, which tells it from the other
    // properties an FProperty links to.
    uintptr_t overrides = FindProperty(FindStruct("MeshComponent", "Class"), "OverrideMaterials");
    for (size_t offset = kSubclassBegin; overrides && offset < kSubclassEnd && !innerOffset_; offset += sizeof(uintptr_t)) {
        uintptr_t candidate = 0;
        Field inner;
        if (SafeRead(overrides + offset, &candidate, sizeof candidate) && candidate && candidate != overrides &&
            ReadField(candidate, inner) && inner.name == "OverrideMaterials" && HoldsObject(inner.type)) {
            innerOffset_ = offset;
        }
    }
    // A struct property points at the script struct it holds.
    uintptr_t materials = FindProperty(FindStruct("SkeletalMesh", "Class"), "Materials");
    uintptr_t element = FindStruct("SkeletalMaterial", "ScriptStruct");
    uintptr_t inner = 0;
    if (innerOffset_ && materials && element && SafeRead(materials + innerOffset_, &inner, sizeof inner) && inner) {
        for (size_t offset = kSubclassBegin; offset < kSubclassEnd && !structOffset_; offset += sizeof(uintptr_t)) {
            uintptr_t candidate = 0;
            if (SafeRead(inner + offset, &candidate, sizeof candidate) && candidate == element) {
                structOffset_ = offset;
            }
        }
    }
}

bool Reflection::ReadField(uintptr_t address, Field& field) const {
    uint8_t raw[kPropertyBytes];
    uintptr_t fieldClass = 0;
    uint32_t typeName[2] = {};
    uint32_t name[2] = {};
    if (!address || address % sizeof(uintptr_t) || !SafeRead(address, raw, sizeof raw)) {
        return false;
    }
    memcpy(&fieldClass, raw + kFieldClass, sizeof fieldClass);
    if (!fieldClass || fieldClass % sizeof(uintptr_t) || !SafeRead(fieldClass, typeName, sizeof typeName)) {
        return false;
    }
    memcpy(name, raw + kFieldName, sizeof name);
    memcpy(&field.next, raw + kFieldNext, sizeof field.next);
    memcpy(&field.arrayDim, raw + kArrayDim, sizeof field.arrayDim);
    memcpy(&field.elementSize, raw + kElementSize, sizeof field.elementSize);
    memcpy(&field.offset, raw + kOffsetInternal, sizeof field.offset);
    field.type = system_.Name(typeName[0], typeName[1]);
    field.name = system_.Name(name[0], name[1]);
    return true;
}

uintptr_t Reflection::FindStruct(const char* name, const char* kind) const {
    for (size_t i = 0; i < view_.All().size(); ++i) {
        int32_t index = static_cast<int32_t>(i);
        if (view_.Name(index) == name && view_.ClassName(index) == kind) {
            return view_[index].address;
        }
    }
    return 0;
}

uintptr_t Reflection::FindProperty(uintptr_t owner, const char* propertyName) const {
    for (int parents = 0; owner && parents < kMaxParents; ++parents) {
        uintptr_t property = 0;
        SafeRead(owner + kChildProperties, &property, sizeof property);
        Field field;
        for (int guard = 0; property && guard < kMaxProperties && ReadField(property, field); ++guard) {
            if (field.name == propertyName) {
                return property;
            }
            property = field.next;
        }
        if (!SafeRead(owner + kSuperStruct, &owner, sizeof owner)) {
            break;
        }
    }
    return 0;
}

int32_t Reflection::PropertyOffset(uintptr_t owner, const char* propertyName) const {
    Field field;
    return ReadField(FindProperty(owner, propertyName), field) ? field.offset : -1;
}

uint32_t Reflection::SizeOf(uintptr_t owner) const {
    int32_t size = 0;
    return owner && SafeRead(owner + kPropertiesSize, &size, sizeof size) && size > 0 ? static_cast<uint32_t>(size) : 0;
}

void Reflection::CollectObjectFields(uintptr_t owner, uint32_t base, int depth, std::vector<uint32_t>& fields) const {
    for (int parents = 0; owner && parents < kMaxParents; ++parents) {
        uintptr_t property = 0;
        SafeRead(owner + kChildProperties, &property, sizeof property);
        Field field;
        for (int guard = 0; property && guard < kMaxProperties && ReadField(property, field); ++guard) {
            int32_t copies = std::clamp(field.arrayDim, 1, 64);
            if (field.offset >= 0 && HoldsObject(field.type)) {
                for (int32_t copy = 0; copy < copies; ++copy) {
                    fields.push_back(base + static_cast<uint32_t>(field.offset) + static_cast<uint32_t>(copy) * sizeof(uintptr_t));
                }
            } else if (field.offset >= 0 && field.elementSize > 0 && field.type == "StructProperty" && depth < kMaxStructDepth) {
                uintptr_t nested = 0;
                SafeRead(property + structOffset_, &nested, sizeof nested);
                for (int32_t copy = 0; nested && copy < copies; ++copy) {
                    CollectObjectFields(nested, base + static_cast<uint32_t>(field.offset + copy * field.elementSize), depth + 1, fields);
                }
            }
            property = field.next;
        }
        if (!SafeRead(owner + kSuperStruct, &owner, sizeof owner)) {
            break;
        }
    }
}

const std::vector<ArrayPlan>& Reflection::ArraysOf(uintptr_t klass) {
    auto known = arrays_.find(klass);
    if (known != arrays_.end()) {
        return known->second;
    }
    std::vector<ArrayPlan> plans;
    uintptr_t owner = klass;
    for (int parents = 0; Ready() && owner && parents < kMaxParents; ++parents) {
        uintptr_t property = 0;
        SafeRead(owner + kChildProperties, &property, sizeof property);
        Field field;
        for (int guard = 0; property && guard < kMaxProperties && ReadField(property, field); ++guard) {
            uintptr_t inner = 0;
            Field element;
            if (field.type == "ArrayProperty" && field.offset >= 0 && SafeRead(property + innerOffset_, &inner, sizeof inner) &&
                ReadField(inner, element) && element.elementSize > 0 && element.elementSize <= 0x10000) {
                ArrayPlan plan{static_cast<uint32_t>(field.offset), static_cast<uint32_t>(element.elementSize), {}, field.name};
                if (HoldsObject(element.type)) {
                    plan.fields.push_back(0);
                } else if (element.type == "StructProperty") {
                    uintptr_t nested = 0;
                    SafeRead(inner + structOffset_, &nested, sizeof nested);
                    CollectObjectFields(nested, 0, 0, plan.fields);
                }
                if (!plan.fields.empty()) {
                    plans.push_back(std::move(plan));
                }
            }
            property = field.next;
        }
        if (!SafeRead(owner + kSuperStruct, &owner, sizeof owner)) {
            break;
        }
    }
    return arrays_.emplace(klass, std::move(plans)).first->second;
}

std::vector<ArrayReference> Reflection::FindArrayReferences(const std::vector<int32_t>& targets) {
    std::vector<ArrayReference> references;
    if (!Ready() || targets.empty()) {
        return references;
    }
    // The view is sorted by address, so ascending indices are ascending addresses.
    std::vector<uintptr_t> addresses;
    std::vector<uint8_t> isTarget(view_.All().size(), 0);
    for (int32_t target : targets) {
        addresses.push_back(view_[target].address);
        isTarget[target] = 1;
    }
    uintptr_t low = addresses.front();
    uintptr_t span = addresses.back() - low;

    std::vector<uint8_t> storage;
    for (size_t i = 0; i < view_.All().size(); ++i) {
        if (isTarget[i]) {
            continue;
        }
        const ObjectInfo& object = view_.All()[i];
        for (const ArrayPlan& plan : ArraysOf(object.klass)) {
            ArrayHeader header;
            size_t bytes = 0;
            if (!ReadArrayHeader(object.address + plan.offset, header) ||
                (bytes = static_cast<size_t>(header.count) * plan.elementSize) > kMaxArrayBytes) {
                continue;
            }
            storage.resize(bytes);
            if (!SafeRead(header.data, storage.data(), bytes)) {
                continue;
            }
            for (int32_t element = 0; element < header.count; ++element) {
                for (uint32_t field : plan.fields) {
                    uintptr_t value;
                    if (field + sizeof value > plan.elementSize) {
                        continue;
                    }
                    memcpy(&value, &storage[static_cast<size_t>(element) * plan.elementSize + field], sizeof value);
                    if (value - low > span) {
                        continue;
                    }
                    auto found = std::lower_bound(addresses.begin(), addresses.end(), value);
                    if (found != addresses.end() && *found == value) {
                        references.push_back({static_cast<int32_t>(i), targets[found - addresses.begin()], plan.offset, plan.elementSize,
                                              field, static_cast<uint32_t>(element), plan.name});
                    }
                }
            }
        }
    }
    return references;
}
