#include "reload.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
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
    Rewrite,            // plain data: store the new pointer
    Leave,              // an object that is expected to die with the old assets
};

SlotKind Classify(const ObjectView& view, const Superseded& set, const ReferenceSlot& slot) {
    const ObjectInfo& owner = view[slot.owner];
    // Dynamic materials cache their parent on the render thread, and instances of a superseded class
    // cannot be turned into instances of the new one. Both are recreated when the mesh is re-set.
    bool ownerClassSuperseded = std::binary_search(set.objects.begin(), set.objects.end(), view.IndexOf(owner.klass));
    if (ownerClassSuperseded || view.IsA(slot.owner, "MaterialInstanceDynamic")) {
        return SlotKind::Leave;
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
    Append(report, "新包里的包 %zu 个，其中已加载 %zu 个，旧对象共 %zu 个，其他对象里指向它们的槽位 %zu 处", packageIds.size(),
           set.packages.size(), set.objects.size(), set.slots.size());

    Append(report, "---- 第一步: 给这些旧包改名 ----");
    for (int32_t package : set.packages) {
        Append(report, "%#zx  %s", view[package].address, view.Name(package).c_str());
    }

    Append(report, "---- 第二步: 重新加载这些资产（包里的顶层公开对象）----");
    for (int32_t object : set.objects) {
        const ObjectInfo& info = view[object];
        int32_t outer = view.IndexOf(info.outer);
        if (outer >= 0 && view[outer].outer == 0 && (info.objectFlags & kObjectPublic) && !(info.objectFlags & kObjectClassDefault)) {
            Append(report, "%#zx  %s  %s", info.address, view.ClassName(object).c_str(), view.Path(object).c_str());
        }
    }

    Append(report, "---- 第三步: 处理指向旧对象的槽位 ----");
    for (const ReferenceSlot& slot : set.slots) {
        SlotKind kind = Classify(view, set, slot);
        Append(report, "%s  %s %s +%#zx（标志 %#x）-> %s %s",
               kind == SlotKind::LiveMeshComponent ? "[重设网格]" : kind == SlotKind::Rewrite ? "[改写]    " : "[不动]    ",
               view.ClassName(slot.owner).c_str(), view.Path(slot.owner).c_str(), slot.location - view[slot.owner].address,
               view[slot.owner].objectFlags, view.ClassName(slot.target).c_str(), view.Name(slot.target).c_str());
    }
}
