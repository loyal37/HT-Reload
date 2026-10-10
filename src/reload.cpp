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
        case SlotKind::LiveMeshComponent: return "[重设网格]";
        case SlotKind::DynamicMaterial:   return "[换父材质]";
        case SlotKind::Rewrite:           return "[改写]    ";
        case SlotKind::Doubtful:          return "[存疑]    ";
        default:                          return "[不动]    ";
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

// Offset of the reflected property `propertyName` that the class `className` declares, or -1.
// UStruct::ChildProperties is at 0x50; FField has its next at 0x18 and name at 0x20; FProperty its offset at 0x44.
int32_t PropertyOffset(const ObjectSystem& system, const ObjectView& view, const char* className, const char* propertyName) {
    for (size_t i = 0; i < view.All().size(); ++i) {
        int32_t index = static_cast<int32_t>(i);
        if (view.Name(index) != className || view.ClassName(index) != "Class") {
            continue;
        }
        uintptr_t property = 0;
        SafeRead(view[index].address + 0x50, &property, sizeof property);
        for (int guard = 0; property && guard < 4000; ++guard) {
            uint8_t raw[0x48];
            if (!SafeRead(property, raw, sizeof raw)) {
                break;
            }
            uint32_t name[2];
            int32_t offset;
            memcpy(name, raw + 0x20, sizeof name);
            memcpy(&offset, raw + 0x44, sizeof offset);
            if (system.Name(name[0], name[1]) == propertyName) {
                return offset;
            }
            memcpy(&property, raw + 0x18, sizeof property);
        }
    }
    return -1;
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
    bool inLevel = false;
    for (int32_t outer = view.IndexOf(owner.outer), depth = 0; outer >= 0 && depth < 32; ++depth) {
        inLevel = inLevel || view.IsA(outer, "Level");
        outer = view.IndexOf(view[outer].outer);
    }
    bool isTemplate = (owner.objectFlags & (kObjectClassDefault | kObjectArchetype)) != 0;
    if (inLevel && !isTemplate && view.IsA(slot.owner, "SkinnedMeshComponent") && view.ClassName(slot.target) == "SkeletalMesh") {
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

// Everything a reload does, worked out ahead of time so that the game thread only has to carry it out.
struct ReloadWork {
    struct Package {
        uintptr_t address;
        std::string name;
        std::wstring retiredName;
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
    };
    std::vector<Package> packages;
    std::vector<Asset> assets;
    std::vector<Slot> slots;
};

ReloadWork PrepareWork(const ObjectSystem& system, const ObjectView& view, const Superseded& set) {
    ReloadWork work;
    char suffix[32];
    snprintf(suffix, sizeof suffix, "%s%llx", kRetiredMark, GetTickCount64());
    for (int32_t package : set.packages) {
        std::string name = view.Name(package);
        work.packages.push_back({view[package].address, name, Widen(name + suffix)});
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
    int32_t parentOffset = PropertyOffset(system, view, "MaterialInstance", "Parent");
    for (const ReferenceSlot& slot : set.slots) {
        int32_t asset = -1;
        for (size_t i = 0; i < work.assets.size(); ++i) {
            if (work.assets[i].index == slot.target) {
                asset = static_cast<int32_t>(i);
            }
        }
        char offset[32];
        snprintf(offset, sizeof offset, " +%#zx", slot.location - view[slot.owner].address);
        work.slots.push_back({Classify(system, view, set, slot, parentOffset), slot.location, view[slot.owner].address,
                              view[slot.target].address, asset,
                              view.ClassName(slot.owner) + " " + view.Path(slot.owner) + offset + " -> " + view.ClassName(slot.target) +
                                  " " + view.Name(slot.target)});
        if (work.slots.back().kind == SlotKind::Doubtful) {
            work.slots.back().outcome = "可能是名字";
        }
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
        Append(report, "试调用: 条件不满足（函数 %#zx，对象 %#zx，参数大小 %u）", function, library, calls_.ParamsSize(function));
        return;
    }
    // int64 UKismetSystemLibrary::GetFrameCount(): no arguments, the return value is the whole parameter block.
    for (int round = 0; round < 2; ++round) {
        int64_t frame = -1;
        DWORD exceptionCode = 0;
        bool called = false;
        bool ran = GameThread_RunAndWait([&] { called = CallProcessEvent(calls_, library, function, &frame, &exceptionCode); }, 10000);
        Append(report, "试调用 GetFrameCount 第 %d 次: %s，返回 %lld，异常码 %#lx", round + 1,
               !ran ? "游戏线程没有执行" : called ? "成功" : "引擎抛出异常", frame, exceptionCode);
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
        Append(report, "条件不满足（资产 %d，库对象 %#zx，函数 %#zx，参数大小 %#x）", asset, library, loadAsset, calls_.ParamsSize(loadAsset));
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

    Append(report, "旧资产 %#zx，旧包 %#zx %s", oldAsset, oldPackage, packageName.c_str());
    if (!ran) {
        Append(report, "游戏线程没有执行");
        return;
    }
    Append(report, "改名: %s，返回 %d，异常码 %#lx", renameCalled ? "已调用" : "引擎抛出异常", renamed, renameException);
    Append(report, "加载: %s，返回对象 %#zx，异常码 %#lx", loadCalled ? "已调用" : "未调用或引擎抛出异常", params.returnValue, loadException);

    ObjectView after(objects_);
    int32_t oldPackageNow = after.IndexOf(oldPackage);
    int32_t loaded = after.IndexOf(params.returnValue);
    Append(report, "旧包现在叫: %s", after.Name(oldPackageNow).c_str());
    Append(report, "旧资产现在是: %s %s", after.ClassName(after.IndexOf(oldAsset)).c_str(), after.Path(after.IndexOf(oldAsset)).c_str());
    Append(report, "新加载的对象: %s %s%s", after.ClassName(loaded).c_str(), after.Path(loaded).c_str(),
           params.returnValue == oldAsset ? "（和旧资产是同一个，没有重新加载）" : "");
}

void Reloader::Plan(const std::vector<uint64_t>& packageIds, std::string& report) {
    ObjectView view(objects_);
    Superseded set = FindSuperseded(view, packageIds, true);
    ReloadWork work = PrepareWork(objects_, view, set);
    Append(report, "新包里的包 %zu 个，其中已加载 %zu 个，旧对象共 %zu 个，其他对象里指向它们的槽位 %zu 处", packageIds.size(),
           set.packages.size(), set.objects.size(), set.slots.size());
    size_t nameLike = 0;
    for (int32_t object : set.objects) {
        nameLike += ReadsAsName(objects_, view[object].address);
    }
    Append(report, "其中 %zu 个旧对象的地址同时也是一个名字的编号，指向它们的槽位只认类里声明过的对象引用", nameLike);

    Append(report, "---- 第一步: 给这些旧包改名 ----");
    for (const ReloadWork::Package& package : work.packages) {
        Append(report, "%#zx  %s", package.address, package.name.c_str());
    }
    Append(report, "---- 第二步: 重新加载这些资产（包里的顶层公开对象）----");
    for (const ReloadWork::Asset& asset : work.assets) {
        Append(report, "%#zx  %s", asset.oldAddress, asset.label.c_str());
    }
    Append(report, "---- 第三步: 处理指向旧对象的槽位 ----");
    for (const ReloadWork::Slot& slot : work.slots) {
        Append(report, "%s  %s", Label(slot.kind), slot.label.c_str());
    }
}

ReloadResult Reloader::Reload(const std::vector<uint64_t>& packageIds, std::string& report) {
    if (!calls_.Ready() && !Resolve(report)) {
        return ReloadResult::Failed;
    }
    ObjectView view(objects_);
    Superseded set = FindSuperseded(view, packageIds, true);
    ReloadWork work = PrepareWork(objects_, view, set);
    if (work.packages.empty()) {
        Append(report, "这些包当前都没有被加载，不需要重载");
        return ReloadResult::Nothing;
    }

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
        Append(report, "条件不满足: 旧包 %zu 个，资产 %zu 个，库对象 %#zx，加载 %#zx，设网格 %#zx，材质同步 %#zx", work.packages.size(),
               work.assets.size(), library, loadAsset, setMesh, copyOverrides);
        return ReloadResult::Failed;
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
    bool ran = GameThread_RunAndWait(
        [&] {
            for (ReloadWork::Package& package : work.packages) {
                if (!CallRename(calls_, package.address, package.retiredName.c_str(), &package.renamed, &exceptionCode) ||
                    !package.renamed) {
                    stopped = "改名";
                    return;
                }
            }
            for (ReloadWork::Asset& asset : work.assets) {
                if (!call(library, loadAsset, &asset.params, "加载")) {
                    return;
                }
            }
            auto replacement = [&](const ReloadWork::Slot& slot) -> uintptr_t {
                uintptr_t loaded = slot.asset < 0 ? 0 : work.assets[slot.asset].params.returnValue;
                return loaded != slot.oldTarget ? loaded : 0;
            };
            // Plain data first, so that whatever the engine and the game do while the mesh is re-set
            // already sees the new assets everywhere else.
            for (ReloadWork::Slot& slot : work.slots) {
                if (stopped) {
                    return;
                }
                uintptr_t loaded = replacement(slot);
                if (slot.kind == SlotKind::Rewrite) {
                    slot.outcome = !loaded ? "没有对应的新资产" : ReplacePointer(slot.location, slot.oldTarget, loaded) ? "已改写" : "内容已变，未改";
                } else if (slot.kind == SlotKind::DynamicMaterial) {
                    if (!loaded || !ReplacePointer(slot.location, slot.oldTarget, loaded)) {
                        slot.outcome = "未改";
                    } else if (call(slot.owner, copyOverrides, &loaded, "材质同步")) {
                        slot.outcome = "已换父材质";
                    }
                }
            }
            // A component has several slots for its mesh; re-setting it once covers them all.
            uintptr_t lastComponent = 0;
            for (ReloadWork::Slot& slot : work.slots) {
                uintptr_t loaded = replacement(slot);
                if (slot.kind != SlotKind::LiveMeshComponent) {
                    continue;
                }
                if (slot.owner == lastComponent) {
                    slot.outcome = "同上";
                } else if (!loaded) {
                    slot.outcome = "没有对应的新资产";
                } else if (call(slot.owner, setMesh, &loaded, "重设网格")) {
                    slot.outcome = "已重设";
                    lastComponent = slot.owner;
                }
            }
            // The game's component keeps the mesh in fields of its own as well. Should re-setting have
            // left one of them alone, it would be the only thing still pointing at the old mesh.
            for (ReloadWork::Slot& slot : work.slots) {
                uintptr_t loaded = replacement(slot);
                if (!stopped && slot.kind == SlotKind::LiveMeshComponent && loaded && ReplacePointer(slot.location, slot.oldTarget, loaded)) {
                    slot.outcome = "重设后仍是旧值，已改写";
                }
            }
        },
        120000);

    if (!ran) {
        Append(report, "游戏线程没有执行");
        return ReloadResult::Failed;
    }
    size_t renamed = 0;
    size_t loaded = 0;
    for (const ReloadWork::Package& package : work.packages) {
        renamed += package.renamed;
    }
    for (const ReloadWork::Asset& asset : work.assets) {
        loaded += asset.params.returnValue && asset.params.returnValue != asset.oldAddress;
    }
    Append(report, "改名 %zu/%zu 个包，重新加载出新对象 %zu/%zu 个", renamed, work.packages.size(), loaded, work.assets.size());
    if (stopped) {
        Append(report, "中止于“%s”，异常码 %#lx。之后的步骤没有执行", stopped, exceptionCode);
    }
    for (const ReloadWork::Asset& asset : work.assets) {
        if (!asset.params.returnValue || asset.params.returnValue == asset.oldAddress) {
            Append(report, "没有加载出新对象: %s（返回 %#zx）", asset.label.c_str(), asset.params.returnValue);
        }
    }
    for (const ReloadWork::Slot& slot : work.slots) {
        Append(report, "%s %-12s %s", Label(slot.kind), slot.outcome, slot.label.c_str());
    }
    return stopped ? ReloadResult::Failed : ReloadResult::Done;
}
