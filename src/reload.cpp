#include "reload.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "game_thread.h"
#include "probe.h"
#include "reflect.h"

namespace {

// Appended to the name of a package that has been superseded, to get it out of the way of the new one.
const char kRetiredMark[] = "_HTOLD_";

void Append(std::string& out, const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (length > 0) {
        out.append(text, std::min<size_t>(static_cast<size_t>(length), sizeof text - 1));
        out += "\r\n";
    }
}

// How a slot that points at an old asset has to be dealt with.
enum class SlotKind {
    LiveMeshComponent,  // re-set the mesh through the engine so render state and anim instances are rebuilt
    DynamicMaterial,    // store the new parent, then have the material re-send its state to the renderer
    Rewrite,            // plain data: store the new pointer
    Leave,              // an object that is expected to die with the old assets
    Doubtful,           // the stored value may be a name that happens to equal the address: never written
};

const char* Label(SlotKind kind) {
    switch (kind) {
        case SlotKind::LiveMeshComponent: return "[reset mesh]";
        case SlotKind::DynamicMaterial:   return "[reparent]  ";
        case SlotKind::Rewrite:           return "[rewrite]   ";
        case SlotKind::Doubtful:          return "[doubtful]  ";
        default:                          return "[leave]     ";
    }
}

// Whether the address of an object could just as well be a stored name: a name is kept as the id of
// its pool entry in the low half and a number in the high half, and the ids reach into the range of
// low heap addresses.
bool ReadsAsName(const ObjectSystem& system, uintptr_t address) {
    return system.IsNameId(static_cast<uint32_t>(address));
}

// Whether the class at `klass`, or one it derives from, declares an object reference at `offset`.
// UStruct: SuperStruct at 0x40, ChildProperties at 0x50. FField: class at 0x08 (which starts with its
// name), next at 0x18. FProperty: array size at 0x30, element size at 0x34, offset at 0x44.
bool DeclaresObjectAt(const ObjectSystem& system, uintptr_t klass, size_t offset) {
    for (int depth = 0; klass && depth < 64; ++depth) {
        uintptr_t property = 0;
        SafeRead(klass + 0x50, &property, sizeof property);
        for (int guard = 0; property && guard < 4000; ++guard) {
            uint8_t raw[0x48];
            if (!SafeRead(property, raw, sizeof raw)) {
                break;
            }
            uintptr_t fieldClass;
            int32_t arraySize;
            int32_t elementSize;
            int32_t start;
            memcpy(&fieldClass, raw + 0x08, sizeof fieldClass);
            memcpy(&arraySize, raw + 0x30, sizeof arraySize);
            memcpy(&elementSize, raw + 0x34, sizeof elementSize);
            memcpy(&start, raw + 0x44, sizeof start);
            if (start >= 0 && arraySize >= 1 && elementSize == sizeof(uintptr_t) && offset >= static_cast<size_t>(start) &&
                offset < static_cast<size_t>(start) + sizeof(uintptr_t) * static_cast<size_t>(arraySize) &&
                (offset - static_cast<size_t>(start)) % sizeof(uintptr_t) == 0) {
                uint32_t typeName[2] = {};
                SafeRead(fieldClass, typeName, sizeof typeName);
                std::string type = system.Name(typeName[0], typeName[1]);
                return type == "ObjectProperty" || type == "ClassProperty" || type == "ObjectPtrProperty" || type == "ClassPtrProperty";
            }
            memcpy(&property, raw + 0x18, sizeof property);
        }
        if (!SafeRead(klass + 0x40, &klass, sizeof klass)) {
            break;
        }
    }
    return false;
}

// Whether the object sits in a level as a working object, as opposed to a class default or a template.
bool IsLive(const ObjectView& view, int32_t object) {
    if (view[object].objectFlags & (kObjectClassDefault | kObjectArchetype)) {
        return false;
    }
    for (int32_t outer = view.IndexOf(view[object].outer), depth = 0; outer >= 0 && depth < 32; ++depth) {
        if (view.IsA(outer, "Level")) {
            return true;
        }
        outer = view.IndexOf(view[outer].outer);
    }
    return false;
}

// How a reference held in the storage of an array has to be dealt with.
enum class ArrayKind {
    Rewrite,           // plain data: store the new pointer in the element
    OverrideMaterial,  // an override material of a live mesh component: set through the engine
    DynamicTexture,    // a texture parameter of a dynamic material: set through the engine
    Leave,             // an object that is expected to die with the old assets
};

const char* Label(ArrayKind kind) {
    switch (kind) {
        case ArrayKind::Rewrite:          return "[array]     ";
        case ArrayKind::OverrideMaterial: return "[material]  ";
        case ArrayKind::DynamicTexture:   return "[texture]   ";
        default:                          return "[leave]     ";
    }
}

ArrayKind Classify(const ObjectView& view, const Superseded& set, const ArrayReference& reference) {
    if (std::binary_search(set.objects.begin(), set.objects.end(), view.IndexOf(view[reference.owner].klass))) {
        return ArrayKind::Leave;
    }
    if (view.IsA(reference.owner, "MaterialInstanceDynamic")) {
        return reference.property == "TextureParameterValues" ? ArrayKind::DynamicTexture : ArrayKind::Rewrite;
    }
    if (reference.property == "OverrideMaterials" && view.IsA(reference.owner, "MeshComponent") && IsLive(view, reference.owner)) {
        return ArrayKind::OverrideMaterial;
    }
    return ArrayKind::Rewrite;
}

// The package of a material instance that was loaded from disk, -1 for anything else. The render
// state of such a material is built when it is loaded and cannot be brought up to date from outside,
// so one that refers to a superseded asset is reloaded as a whole instead of being patched.
int32_t ReloadableMaterialPackage(const ObjectView& view, int32_t object) {
    if (!view.IsA(object, "MaterialInstance") || view.IsA(object, "MaterialInstanceDynamic") ||
        !(view[object].objectFlags & kObjectPublic)) {
        return -1;
    }
    int32_t package = view.IndexOf(view[object].outer);
    if (package < 0 || view[package].outer != 0) {
        return -1;
    }
    std::string name = view.Name(package);
    return name.rfind("/Game/", 0) == 0 && name.find(kRetiredMark) == std::string::npos ? package : -1;
}

constexpr size_t kMaxDependents = 64;
constexpr int kMaxRounds = 8;

// What a set of containers supersedes, widened by the material instances that refer to it.
struct Gathered {
    Superseded set;
    std::vector<ArrayReference> arrays;
    std::vector<int32_t> dependents;  // packages added because a material instance in them refers to a superseded asset
    bool capped = false;              // more of them were found than are taken along
};

Gathered Gather(const ObjectView& view, Reflection& reflection, const std::vector<uint64_t>& packageIds) {
    Gathered gathered;
    for (int round = 0;; ++round) {
        gathered.set = FindSuperseded(view, packageIds, gathered.dependents, true);
        gathered.arrays = reflection.FindArrayReferences(gathered.set.objects);
        std::vector<int32_t> added;
        auto consider = [&](int32_t owner) {
            int32_t package = ReloadableMaterialPackage(view, owner);
            if (package >= 0 && std::find(added.begin(), added.end(), package) == added.end()) {
                added.push_back(package);
            }
        };
        for (const ReferenceSlot& slot : gathered.set.slots) {
            consider(slot.owner);
        }
        for (const ArrayReference& reference : gathered.arrays) {
            consider(reference.owner);
        }
        if (added.empty()) {
            break;
        }
        if (round >= kMaxRounds || gathered.dependents.size() + added.size() > kMaxDependents) {
            gathered.capped = true;
            break;
        }
        gathered.dependents.insert(gathered.dependents.end(), added.begin(), added.end());
    }
    return gathered;
}

SlotKind Classify(const ObjectSystem& system, const ObjectView& view, const Superseded& set, const ReferenceSlot& slot,
                  int32_t materialParentOffset) {
    const ObjectInfo& owner = view[slot.owner];
    // An instance of a superseded class cannot be turned into an instance of the new one. The anim
    // instances among them are recreated when the mesh is re-set.
    if (std::binary_search(set.objects.begin(), set.objects.end(), view.IndexOf(owner.klass))) {
        return SlotKind::Leave;
    }
    // Slots are found by their value alone. Where that value also reads as a name, only a declared
    // object reference is taken for a pointer: storing an address over a name would corrupt the owner.
    if (ReadsAsName(system, view[slot.target].address) && !DeclaresObjectAt(system, owner.klass, slot.location - owner.address)) {
        return SlotKind::Doubtful;
    }
    // The game keeps its dynamic materials in containers of its own, so they have to stay the same
    // objects and take the new material as their parent.
    if (view.IsA(slot.owner, "MaterialInstanceDynamic")) {
        bool isParent = materialParentOffset >= 0 && slot.location == owner.address + static_cast<uintptr_t>(materialParentOffset);
        return isParent ? SlotKind::DynamicMaterial : SlotKind::Leave;
    }
    // Only a component that sits in a level has render state and anim instances to rebuild; the ones in
    // class defaults and in sequence templates are plain data.
    if (IsLive(view, slot.owner) && view.IsA(slot.owner, "SkinnedMeshComponent") && view.ClassName(slot.target) == "SkeletalMesh") {
        return SlotKind::LiveMeshComponent;
    }
    return SlotKind::Rewrite;
}

// Parameter block of UKismetSystemLibrary::LoadAsset_Blocking(TSoftObjectPtr<UObject>) -> UObject*.
struct LoadAssetParams {
    // TSoftObjectPtr: a weak pointer cache, then the FSoftObjectPath.
    int32_t weakIndex = 0;
    int32_t weakSerial = 0;
    uint32_t packageNameId = 0;  // FTopLevelAssetPath::PackageName
    uint32_t packageNameNumber = 0;
    uint32_t assetNameId = 0;  // FTopLevelAssetPath::AssetName
    uint32_t assetNameNumber = 0;
    void* subPathData = nullptr;  // empty sub-path string
    int32_t subPathNum = 0;
    int32_t subPathMax = 0;
    uintptr_t returnValue = 0;
};
static_assert(sizeof(LoadAssetParams) == 0x30 && offsetof(LoadAssetParams, returnValue) == 0x28);

std::wstring Widen(const std::string& text) {
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

// Stores `value` in a pointer slot if it still holds `expected`.
bool ReplacePointer(uintptr_t location, uintptr_t expected, uintptr_t value) {
    __try {
        uintptr_t* slot = reinterpret_cast<uintptr_t*>(location);
        if (*slot != expected) {
            return false;
        }
        *slot = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Calls visit(element index, address of the field) for every element of a TArray whose field holds `value`.
template <class Visit>
void ForEachElementHolding(uintptr_t arrayAddress, uint32_t elementSize, uint32_t fieldOffset, uintptr_t value, Visit&& visit) {
    ArrayHeader header;
    if (!ReadArrayHeader(arrayAddress, header)) {
        return;
    }
    for (int32_t index = 0; index < header.count; ++index) {
        uintptr_t field = header.data + static_cast<uintptr_t>(index) * elementSize + fieldOffset;
        uintptr_t current = 0;
        if (SafeRead(field, &current, sizeof current) && current == value) {
            visit(index, field);
        }
    }
}

// Where a skeletal mesh keeps the material of each slot, and where a component and a material
// instance keep what overrides it. All looked up through reflection.
struct MaterialLayout {
    int32_t meshMaterials = -1;      // USkeletalMesh::Materials
    uint32_t materialStride = 0;     // sizeof(FSkeletalMaterial)
    int32_t materialInterface = -1;  // FSkeletalMaterial::MaterialInterface
    int32_t overrides = -1;          // UMeshComponent::OverrideMaterials
    int32_t parent = -1;             // UMaterialInstance::Parent
    int32_t textureInfo = -1;        // FTextureParameterValue::ParameterInfo
    uintptr_t dynamicClass = 0;      // the class MaterialInstanceDynamic

    bool KnowsSlots() const {
        return meshMaterials >= 0 && materialStride && materialInterface >= 0 && overrides >= 0 && parent >= 0 && dynamicClass;
    }
};

MaterialLayout FindMaterialLayout(const Reflection& reflection) {
    MaterialLayout layout;
    uintptr_t slotStruct = reflection.FindStruct("SkeletalMaterial", "ScriptStruct");
    layout.meshMaterials = reflection.PropertyOffset(reflection.FindStruct("SkeletalMesh", "Class"), "Materials");
    layout.materialStride = reflection.SizeOf(slotStruct);
    layout.materialInterface = reflection.PropertyOffset(slotStruct, "MaterialInterface");
    layout.overrides = reflection.PropertyOffset(reflection.FindStruct("MeshComponent", "Class"), "OverrideMaterials");
    layout.parent = reflection.PropertyOffset(reflection.FindStruct("MaterialInstance", "Class"), "Parent");
    layout.textureInfo = reflection.PropertyOffset(reflection.FindStruct("TextureParameterValue", "ScriptStruct"), "ParameterInfo");
    layout.dynamicClass = reflection.FindStruct("MaterialInstanceDynamic", "Class");
    return layout;
}

// The material of every slot of a skeletal mesh.
std::vector<uintptr_t> SlotMaterials(const MaterialLayout& layout, uintptr_t mesh) {
    std::vector<uintptr_t> materials;
    ArrayHeader header;
    if (ReadArrayHeader(mesh + layout.meshMaterials, header) && header.count <= 4096) {
        for (int32_t slot = 0; slot < header.count; ++slot) {
            uintptr_t material = 0;
            SafeRead(header.data + static_cast<uintptr_t>(slot) * layout.materialStride + layout.materialInterface, &material,
                     sizeof material);
            materials.push_back(material);
        }
    }
    return materials;
}

// Everything a reload does, worked out ahead of time so that the game thread only has to carry it out.
struct ReloadWork {
    struct Package {
        uintptr_t address;
        std::string name;
        std::wstring retiredName;
        bool dependent = false;  // not in the new container: reloaded because it refers to something that is
        bool renamed = false;
    };
    struct Asset {
        int32_t index;  // in the view
        uintptr_t oldAddress;
        LoadAssetParams params;
        std::string label;
    };
    struct Slot {
        SlotKind kind;
        uintptr_t location;
        uintptr_t owner;
        uintptr_t oldTarget;
        int32_t asset;  // which asset the slot points at, or -1 if it is not one of the reloaded assets
        std::string label;
        const char* outcome = "";
        bool settled = false;  // dealt with, or found to need nothing
    };
    struct Array {
        ArrayKind kind;
        uintptr_t owner;
        uint32_t arrayOffset;
        uint32_t elementSize;
        uint32_t fieldOffset;
        uintptr_t oldTarget;
        int32_t asset;
        std::string label;
        size_t elements = 1;  // how many elements held the reference when the reload was planned
        const char* outcome = "";
        bool settled = false;
    };
    std::vector<Package> packages;
    std::vector<Asset> assets;
    std::vector<Slot> slots;
    std::vector<Array> arrays;
    std::vector<std::string> notes;  // what was done to follow a mesh's changed slot materials
    MaterialLayout layout;
    bool capped = false;
};

ReloadWork PrepareWork(const ObjectSystem& system, const ObjectView& view, const Reflection& reflection, const Gathered& gathered) {
    const Superseded& set = gathered.set;
    ReloadWork work;
    work.layout = FindMaterialLayout(reflection);
    work.capped = gathered.capped;
    char suffix[32];
    snprintf(suffix, sizeof suffix, "%s%llx", kRetiredMark, GetTickCount64());
    for (int32_t package : set.packages) {
        std::string name = view.Name(package);
        bool dependent = std::find(gathered.dependents.begin(), gathered.dependents.end(), package) != gathered.dependents.end();
        work.packages.push_back({view[package].address, name, Widen(name + suffix), dependent});
    }
    // The assets to load are the public objects directly inside the packages, class defaults excepted:
    // those come along with their class.
    for (int32_t object : set.objects) {
        const ObjectInfo& info = view[object];
        int32_t package = view.IndexOf(info.outer);
        if (package < 0 || view[package].outer != 0 || !(info.objectFlags & kObjectPublic) || (info.objectFlags & kObjectClassDefault)) {
            continue;
        }
        ReloadWork::Asset asset{object, info.address, {}, view.ClassName(object) + " " + view.Name(object)};
        asset.params.packageNameId = view[package].nameId;
        asset.params.packageNameNumber = view[package].nameNumber;
        asset.params.assetNameId = info.nameId;
        asset.params.assetNameNumber = info.nameNumber;
        work.assets.push_back(asset);
    }
    auto assetOf = [&](int32_t target) {
        for (size_t i = 0; i < work.assets.size(); ++i) {
            if (work.assets[i].index == target) {
                return static_cast<int32_t>(i);
            }
        }
        return -1;
    };
    for (const ReferenceSlot& slot : set.slots) {
        char offset[32];
        snprintf(offset, sizeof offset, " +%#zx", slot.location - view[slot.owner].address);
        work.slots.push_back({Classify(system, view, set, slot, work.layout.parent), slot.location, view[slot.owner].address,
                              view[slot.target].address, assetOf(slot.target),
                              view.ClassName(slot.owner) + " " + view.Path(slot.owner) + offset + " -> " + view.ClassName(slot.target) +
                                  " " + view.Name(slot.target)});
        if (work.slots.back().kind == SlotKind::Doubtful) {
            work.slots.back().outcome = "may be a name";
        }
    }
    // Several elements of one array can hold the same object; they are dealt with together.
    for (const ArrayReference& reference : gathered.arrays) {
        uintptr_t owner = view[reference.owner].address;
        uintptr_t target = view[reference.target].address;
        auto same = std::find_if(work.arrays.begin(), work.arrays.end(), [&](const ReloadWork::Array& array) {
            return array.owner == owner && array.arrayOffset == reference.arrayOffset && array.fieldOffset == reference.fieldOffset &&
                   array.oldTarget == target;
        });
        if (same != work.arrays.end()) {
            ++same->elements;
            continue;
        }
        char place[48];
        snprintf(place, sizeof place, "[%u]+%#x", reference.index, reference.fieldOffset);
        work.arrays.push_back({Classify(view, set, reference), owner, reference.arrayOffset, reference.elementSize,
                               reference.fieldOffset, target, assetOf(reference.target),
                               view.ClassName(reference.owner) + " " + view.Path(reference.owner) + " " + reference.property + place +
                                   " -> " + view.ClassName(reference.target) + " " + view.Name(reference.target)});
    }
    return work;
}

}  // namespace

bool Reloader::Resolve(std::string& report) {
    return calls_.Resolve(image_, objects_, report);
}

void Reloader::CallTest(std::string& report) {
    if (!calls_.Ready() && !Resolve(report)) {
        return;
    }
    ObjectView view(objects_);
    uintptr_t function = FindFunction(objects_, view.All(), "KismetSystemLibrary", "GetFrameCount");
    uintptr_t library = 0;
    for (size_t i = 0; i < view.All().size(); ++i) {
        if (view.Name(static_cast<int32_t>(i)) == "Default__KismetSystemLibrary") {
            library = view.All()[i].address;
        }
    }
    if (!function || !library || calls_.ParamsSize(function) != sizeof(int64_t)) {
        Append(report, "Call test: preconditions not met (function %#zx, object %#zx, parameter size %u)", function, library, calls_.ParamsSize(function));
        return;
    }
    // int64 UKismetSystemLibrary::GetFrameCount(): no arguments, the return value is the whole parameter block.
    for (int round = 0; round < 2; ++round) {
        int64_t frame = -1;
        DWORD exceptionCode = 0;
        bool called = false;
        bool ran = GameThread_RunAndWait([&] { called = CallProcessEvent(calls_, library, function, &frame, &exceptionCode); }, 10000);
        Append(report, "Call test, GetFrameCount call %d: %s, returned %lld, exception code %#lx", round + 1,
               !ran ? "the game thread did not run it" : called ? "ok" : "the engine raised an exception", frame, exceptionCode);
        Sleep(500);
    }
}

size_t Reloader::RetiredPackagesAlive() const {
    ObjectView view(objects_);
    size_t alive = 0;
    for (size_t i = 0; i < view.All().size(); ++i) {
        int32_t index = static_cast<int32_t>(i);
        alive += view[index].outer == 0 && view.Name(index).find(kRetiredMark) != std::string::npos;
    }
    return alive;
}

void Reloader::RenameAndLoad(const std::string& assetName, std::string& report) {
    if (!calls_.Ready() && !Resolve(report)) {
        return;
    }
    ObjectView view(objects_);
    int32_t asset = -1;
    uintptr_t library = 0;
    for (size_t i = 0; i < view.All().size(); ++i) {
        int32_t index = static_cast<int32_t>(i);
        int32_t outer = view.IndexOf(view[index].outer);
        // A package retired by an earlier run still holds an asset of the same name; skip it.
        if (outer >= 0 && view[outer].outer == 0 && (view[index].objectFlags & kObjectPublic) && view.Name(index) == assetName &&
            view.Name(outer).find(kRetiredMark) == std::string::npos) {
            asset = index;
        }
        if (view.Name(index) == "Default__KismetSystemLibrary") {
            library = view[index].address;
        }
    }
    uintptr_t loadAsset = FindFunction(objects_, view.All(), "KismetSystemLibrary", "LoadAsset_Blocking");
    if (asset < 0 || !library || !loadAsset || calls_.ParamsSize(loadAsset) != sizeof(LoadAssetParams)) {
        Append(report, "Preconditions not met (asset %d, library object %#zx, function %#zx, parameter size %#x)", asset, library, loadAsset, calls_.ParamsSize(loadAsset));
        return;
    }

    int32_t package = view.IndexOf(view[asset].outer);
    uintptr_t oldAsset = view[asset].address;
    uintptr_t oldPackage = view[package].address;
    std::string packageName = view.Name(package);
    char suffix[32];
    snprintf(suffix, sizeof suffix, "%s%llx", kRetiredMark, GetTickCount64());
    std::wstring retiredName = Widen(packageName + suffix);

    LoadAssetParams params;
    params.packageNameId = view[package].nameId;
    params.packageNameNumber = view[package].nameNumber;
    params.assetNameId = view[asset].nameId;
    params.assetNameNumber = view[asset].nameNumber;

    bool renamed = false;
    bool renameCalled = false;
    bool loadCalled = false;
    DWORD renameException = 0;
    DWORD loadException = 0;
    bool ran = GameThread_RunAndWait(
        [&] {
            renameCalled = CallRename(calls_, oldPackage, retiredName.c_str(), &renamed, &renameException);
            if (renameCalled && renamed) {
                loadCalled = CallProcessEvent(calls_, library, loadAsset, &params, &loadException);
            }
        },
        60000);

    Append(report, "Old asset %#zx, old package %#zx %s", oldAsset, oldPackage, packageName.c_str());
    if (!ran) {
        Append(report, "the game thread did not run it");
        return;
    }
    Append(report, "Rename: %s, returned %d, exception code %#lx", renameCalled ? "called" : "the engine raised an exception", renamed, renameException);
    Append(report, "Load: %s, returned object %#zx, exception code %#lx", loadCalled ? "called" : "not called, or the engine raised an exception", params.returnValue, loadException);

    ObjectView after(objects_);
    int32_t oldPackageNow = after.IndexOf(oldPackage);
    int32_t loaded = after.IndexOf(params.returnValue);
    Append(report, "The old package is now named: %s", after.Name(oldPackageNow).c_str());
    Append(report, "The old asset is now: %s %s", after.ClassName(after.IndexOf(oldAsset)).c_str(), after.Path(after.IndexOf(oldAsset)).c_str());
    Append(report, "Newly loaded object: %s %s%s", after.ClassName(loaded).c_str(), after.Path(loaded).c_str(),
           params.returnValue == oldAsset ? " (the same object as the old asset: it was not loaded again)" : "");
}

void Reloader::Plan(const std::vector<uint64_t>& packageIds, std::string& report) {
    ObjectView view(objects_);
    Reflection reflection(objects_, view);
    Gathered gathered = Gather(view, reflection, packageIds);
    const Superseded& set = gathered.set;
    ReloadWork work = PrepareWork(objects_, view, reflection, gathered);
    Append(report, "%zu packages in the new containers; %zu loaded packages to reload, %zu of them material instances that refer to the others",
           packageIds.size(), set.packages.size(), gathered.dependents.size());
    Append(report, "%zu old objects, %zu slots in other objects and %zu array elements pointing at them", set.objects.size(),
           set.slots.size(), gathered.arrays.size());
    if (!reflection.Ready()) {
        Append(report, "The layout of array properties could not be worked out; references held in arrays are not looked at");
    }
    if (gathered.capped) {
        Append(report, "More material instances refer to these assets than are reloaded along with them; the rest is patched in place");
    }
    if (!work.layout.KnowsSlots()) {
        Append(report, "Where meshes and components keep their materials could not be worked out; override materials will not follow a changed slot");
    }
    size_t nameLike = 0;
    for (int32_t object : set.objects) {
        nameLike += ReadsAsName(objects_, view[object].address);
    }
    Append(report, "%zu old objects have an address that also reads as a name id; a slot pointing at one of them counts only where the class declares an object reference", nameLike);

    Append(report, "---- Step 1: rename these old packages ----");
    for (const ReloadWork::Package& package : work.packages) {
        Append(report, "%#zx  %s%s", package.address, package.name.c_str(), package.dependent ? "  (refers to the others)" : "");
    }
    Append(report, "---- Step 2: load these assets again (the public top-level objects of the packages) ----");
    for (const ReloadWork::Asset& asset : work.assets) {
        Append(report, "%#zx  %s", asset.oldAddress, asset.label.c_str());
    }
    Append(report, "---- Step 3: deal with the slots that point at old objects ----");
    for (const ReloadWork::Slot& slot : work.slots) {
        Append(report, "%s  %s", Label(slot.kind), slot.label.c_str());
    }
    Append(report, "---- Step 4: deal with the array elements that point at old objects ----");
    for (const ReloadWork::Array& array : work.arrays) {
        Append(report, "%s  %s  (%zu elements)", Label(array.kind), array.label.c_str(), array.elements);
    }
}

namespace {

// Parameter blocks of UPrimitiveComponent::SetMaterial(int32, UMaterialInterface*) and of
// UMaterialInstanceDynamic::SetTextureParameterValueByInfo(const FMaterialParameterInfo&, UTexture*).
struct SetMaterialParams {
    int32_t elementIndex;
    int32_t padding;
    uintptr_t material;
};
struct SetTextureParams {
    uint8_t parameterInfo[0x10];
    uintptr_t value;
};

}  // namespace

ReloadResult Reloader::Reload(const std::vector<uint64_t>& packageIds, std::string& report) {
    if (!calls_.Ready() && !Resolve(report)) {
        return ReloadResult::Failed;
    }
    ObjectView view(objects_);
    Reflection reflection(objects_, view);
    Gathered gathered = Gather(view, reflection, packageIds);
    ReloadWork work = PrepareWork(objects_, view, reflection, gathered);
    if (work.packages.empty()) {
        Append(report, "None of these packages is loaded; nothing to reload");
        return ReloadResult::Nothing;
    }
    const MaterialLayout& layout = work.layout;

    uintptr_t library = 0;
    for (size_t i = 0; i < view.All().size(); ++i) {
        if (view.Name(static_cast<int32_t>(i)) == "Default__KismetSystemLibrary") {
            library = view.All()[i].address;
        }
    }
    uintptr_t loadAsset = FindFunction(objects_, view.All(), "KismetSystemLibrary", "LoadAsset_Blocking");
    uintptr_t setMesh = FindFunction(objects_, view.All(), "SkeletalMeshComponent", "SetSkeletalMeshAsset");
    uintptr_t copyOverrides = FindFunction(objects_, view.All(), "MaterialInstanceDynamic", "CopyParameterOverrides");
    if (work.assets.empty() || !library || calls_.ParamsSize(loadAsset) != sizeof(LoadAssetParams) ||
        calls_.ParamsSize(setMesh) != sizeof(uintptr_t) || calls_.ParamsSize(copyOverrides) != sizeof(uintptr_t)) {
        Append(report, "Preconditions not met: %zu old packages, %zu assets, library object %#zx, load %#zx, set mesh %#zx, material sync %#zx", work.packages.size(),
               work.assets.size(), library, loadAsset, setMesh, copyOverrides);
        return ReloadResult::Failed;
    }
    // These two are only needed for some kinds of reference; without them the pointer is stored directly.
    uintptr_t setMaterial = FindFunction(objects_, view.All(), "PrimitiveComponent", "SetMaterial");
    uintptr_t setTexture = FindFunction(objects_, view.All(), "MaterialInstanceDynamic", "SetTextureParameterValueByInfo");
    if (!setMaterial || calls_.ParamsSize(setMaterial) != sizeof(SetMaterialParams)) {
        setMaterial = 0;
    }
    if (!setTexture || calls_.ParamsSize(setTexture) != sizeof(SetTextureParams) || layout.textureInfo < 0) {
        setTexture = 0;
    }

    // One task on the game thread, so that no garbage collection can run between loading the new
    // assets and pointing the game at them: until then nothing references them.
    const char* stopped = nullptr;
    DWORD exceptionCode = 0;
    auto call = [&](uintptr_t object, uintptr_t function, void* params, const char* what) {
        if (!stopped && !CallProcessEvent(calls_, object, function, params, &exceptionCode)) {
            stopped = what;
        }
        return !stopped;
    };
    // The loaded copy that takes the place of `oldTarget`, 0 if there is none.
    auto replacement = [&](int32_t asset, uintptr_t oldTarget) -> uintptr_t {
        uintptr_t loaded = asset < 0 ? 0 : work.assets[asset].params.returnValue;
        return loaded != oldTarget ? loaded : 0;
    };
    // What an object is now: its reloaded copy, or the object itself if it was not reloaded.
    auto current = [&](uintptr_t object) -> uintptr_t {
        for (const ReloadWork::Asset& asset : work.assets) {
            if (asset.oldAddress == object && asset.params.returnValue) {
                return asset.params.returnValue;
            }
        }
        return object;
    };
    auto nameOf = [&](uintptr_t object) {
        uint32_t name[2] = {};
        return object && SafeRead(object + 0x18, name, sizeof name) ? objects_.Name(name[0], name[1]) : std::string("nothing");
    };
    // A component carries an override material for each slot, made from the material the mesh had
    // in that slot when the game set the character up, and an override wins over the mesh's own
    // material. Where the new mesh uses a different material in a slot, the override has to follow.
    auto followSlots = [&](uintptr_t component, uintptr_t oldMesh, uintptr_t newMesh) {
        ArrayHeader overrides;
        if (!layout.KnowsSlots() || !ReadArrayHeader(component + layout.overrides, overrides)) {
            return;
        }
        std::vector<uintptr_t> before = SlotMaterials(layout, oldMesh);
        std::vector<uintptr_t> after = SlotMaterials(layout, newMesh);
        for (int32_t slot = 0; slot < overrides.count && static_cast<size_t>(slot) < after.size() && !stopped; ++slot) {
            uintptr_t was = static_cast<size_t>(slot) < before.size() ? before[slot] : 0;
            uintptr_t wasNow = current(was);
            uintptr_t wanted = after[slot];
            uintptr_t pinned = 0;
            if (!wanted || wanted == wasNow ||
                !SafeRead(overrides.data + static_cast<uintptr_t>(slot) * sizeof(uintptr_t), &pinned, sizeof pinned) || !pinned ||
                pinned == wanted) {
                continue;
            }
            uintptr_t klass = 0;
            uintptr_t parent = 0;
            char text[512];
            const char* done = nullptr;
            if ((pinned == was || pinned == wasNow) && setMaterial) {
                SetMaterialParams params{slot, 0, wanted};
                done = call(component, setMaterial, &params, "set material") ? "now uses" : nullptr;
            } else if (SafeRead(pinned + 0x10, &klass, sizeof klass) && klass == layout.dynamicClass &&
                       SafeRead(pinned + layout.parent, &parent, sizeof parent) && (parent == was || parent == wasNow)) {
                done = ReplacePointer(pinned + layout.parent, parent, wanted) && call(pinned, copyOverrides, &wanted, "material sync")
                           ? "has its dynamic material based on"
                           : nullptr;
            } else {
                snprintf(text, sizeof text, "Slot %d of %s: the mesh changed from %s to %s, but the override %s is not derived from the old one and was left",
                         slot, nameOf(component).c_str(), nameOf(was).c_str(), nameOf(wanted).c_str(), nameOf(pinned).c_str());
                work.notes.push_back(text);
            }
            if (done) {
                snprintf(text, sizeof text, "Slot %d of %s %s %s (was %s)", slot, nameOf(component).c_str(), done, nameOf(wanted).c_str(),
                         nameOf(was).c_str());
                work.notes.push_back(text);
            }
        }
    };

    bool ran = GameThread_RunAndWait(
        [&] {
            for (ReloadWork::Package& package : work.packages) {
                if (!CallRename(calls_, package.address, package.retiredName.c_str(), &package.renamed, &exceptionCode) ||
                    !package.renamed) {
                    stopped = "rename";
                    return;
                }
            }
            for (ReloadWork::Asset& asset : work.assets) {
                if (!call(library, loadAsset, &asset.params, "load")) {
                    return;
                }
            }
            // Plain data first, so that whatever the engine and the game do while the mesh is re-set
            // already sees the new assets everywhere else.
            for (ReloadWork::Slot& slot : work.slots) {
                if (stopped) {
                    return;
                }
                uintptr_t loaded = replacement(slot.asset, slot.oldTarget);
                if (slot.kind != SlotKind::Rewrite && slot.kind != SlotKind::DynamicMaterial) {
                    continue;
                }
                if (!loaded) {
                    slot.outcome = "no new asset for it";
                } else if (!ReplacePointer(slot.location, slot.oldTarget, loaded)) {
                    slot.outcome = "changed meanwhile, left alone";
                    slot.settled = true;
                } else if (slot.kind == SlotKind::Rewrite) {
                    slot.outcome = "rewritten";
                    slot.settled = true;
                } else if (call(slot.owner, copyOverrides, &loaded, "material sync")) {
                    slot.outcome = "reparented";
                    slot.settled = true;
                }
            }
            for (ReloadWork::Array& array : work.arrays) {
                if (stopped) {
                    return;
                }
                uintptr_t loaded = replacement(array.asset, array.oldTarget);
                if (array.kind == ArrayKind::Leave) {
                    continue;
                }
                if (!loaded) {
                    array.outcome = "no new asset for it";
                    continue;
                }
                size_t replaced = 0;
                bool throughEngine = false;
                ForEachElementHolding(array.owner + array.arrayOffset, array.elementSize, array.fieldOffset, array.oldTarget,
                                      [&](int32_t index, uintptr_t field) {
                                          if (array.kind == ArrayKind::OverrideMaterial && setMaterial) {
                                              SetMaterialParams params{index, 0, loaded};
                                              replaced += call(array.owner, setMaterial, &params, "set material");
                                              throughEngine = true;
                                              return;
                                          }
                                          SetTextureParams params{};
                                          if (array.kind == ArrayKind::DynamicTexture && setTexture &&
                                              SafeRead(field - array.fieldOffset + layout.textureInfo, params.parameterInfo,
                                                       sizeof params.parameterInfo)) {
                                              params.value = loaded;
                                              replaced += call(array.owner, setTexture, &params, "set texture");
                                              throughEngine = true;
                                              return;
                                          }
                                          replaced += ReplacePointer(field, array.oldTarget, loaded);
                                      });
                array.outcome = !replaced ? "gone meanwhile" : throughEngine ? "set through the engine" : "rewritten";
                array.settled = true;
            }
            // A component has several slots for its mesh; re-setting it once covers them all.
            uintptr_t lastComponent = 0;
            for (ReloadWork::Slot& slot : work.slots) {
                uintptr_t loaded = replacement(slot.asset, slot.oldTarget);
                if (slot.kind != SlotKind::LiveMeshComponent) {
                    continue;
                }
                if (slot.owner == lastComponent) {
                    slot.outcome = "as above";
                    slot.settled = true;
                } else if (!loaded) {
                    slot.outcome = "no new asset for it";
                } else if (call(slot.owner, setMesh, &loaded, "mesh reset")) {
                    slot.outcome = "reset";
                    slot.settled = true;
                    lastComponent = slot.owner;
                    followSlots(slot.owner, slot.oldTarget, loaded);
                }
            }
            // The game's component keeps the mesh in fields of its own as well. Should re-setting have
            // left one of them alone, it would be the only thing still pointing at the old mesh.
            for (ReloadWork::Slot& slot : work.slots) {
                uintptr_t loaded = replacement(slot.asset, slot.oldTarget);
                if (!stopped && slot.kind == SlotKind::LiveMeshComponent && loaded && ReplacePointer(slot.location, slot.oldTarget, loaded)) {
                    slot.outcome = "still old after the reset, rewritten";
                }
            }
        },
        120000);

    if (!ran) {
        Append(report, "the game thread did not run it");
        return ReloadResult::Failed;
    }
    size_t renamed = 0;
    size_t dependents = 0;
    size_t loaded = 0;
    for (const ReloadWork::Package& package : work.packages) {
        renamed += package.renamed;
        dependents += package.dependent;
    }
    for (const ReloadWork::Asset& asset : work.assets) {
        loaded += asset.params.returnValue && asset.params.returnValue != asset.oldAddress;
    }
    // What a completed run still left pointing at an old asset.
    size_t unsettled = work.assets.size() - loaded;
    for (const ReloadWork::Slot& slot : work.slots) {
        unsettled += slot.kind != SlotKind::Leave && !slot.settled;
    }
    for (const ReloadWork::Array& array : work.arrays) {
        unsettled += array.kind != ArrayKind::Leave && !array.settled;
    }

    Append(report, "Renamed %zu/%zu packages, loaded %zu/%zu new objects", renamed, work.packages.size(), loaded, work.assets.size());
    if (dependents) {
        Append(report, "%zu of the packages are material instances outside the new container, reloaded because they refer to its assets", dependents);
    }
    if (!reflection.Ready()) {
        Append(report, "The layout of array properties could not be worked out; references held in arrays were not looked at");
    }
    if (work.capped) {
        Append(report, "More material instances refer to these assets than are reloaded along with them; the rest was patched in place");
    }
    if (stopped) {
        Append(report, "Stopped at \"%s\", exception code %#lx. The steps after it were not carried out", stopped, exceptionCode);
    } else if (unsettled) {
        Append(report, "%zu assets or references were not replaced; the list below says which", unsettled);
    }
    for (const ReloadWork::Asset& asset : work.assets) {
        if (!asset.params.returnValue || asset.params.returnValue == asset.oldAddress) {
            Append(report, "No new object loaded: %s (returned %#zx)", asset.label.c_str(), asset.params.returnValue);
        }
    }
    for (const std::string& note : work.notes) {
        Append(report, "%s", note.c_str());
    }
    for (const ReloadWork::Slot& slot : work.slots) {
        Append(report, "%s %-36s %s", Label(slot.kind), slot.outcome, slot.label.c_str());
    }
    for (const ReloadWork::Array& array : work.arrays) {
        Append(report, "%s %-36s %s", Label(array.kind), array.outcome, array.label.c_str());
    }
    return stopped ? ReloadResult::Failed : unsettled ? ReloadResult::Partial : ReloadResult::Done;
}
