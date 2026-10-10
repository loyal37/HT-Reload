// HTReload core: mounts pak mods that appear under <Project>/Content/Paks while the game is running,
// by calling the engine's own FCoreDelegates::MountPak handler on the game thread. Loaded and
// replaced at run time by the shell (see host.h).

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "explore.h"
#include "game_thread.h"
#include "host.h"
#include "image.h"
#include "log.h"
#include "objects.h"
#include "probe.h"
#include "reload.h"
#include "resolver.h"

namespace fs = std::filesystem;

namespace {

const char kVersion[] = "0.7.1";

const CoreHost* g_host = nullptr;
// Copied in CoreStart: the shell advances its own count as soon as CoreStart returns.
uint32_t g_generation = 0;
HANDLE g_stop = nullptr;
HANDLE g_worker = nullptr;

// Sleeps, and returns true if the core has been asked to shut down.
bool WaitStop(DWORD milliseconds) {
    return WaitForSingleObject(g_stop, milliseconds) != WAIT_TIMEOUT;
}

struct Paths {
    fs::path moduleDir;         // where the plugin lives
    fs::path paksDir;           // <Project>/Content/Paks
    std::wstring enginePrefix;  // the same directory the way the engine spells it
};

// Memory layout of FString.
struct EngineString {
    const wchar_t* data;
    int32_t num;
    int32_t max;
};

using HandleMountPakFn = void* (*)(void* self, const EngineString* pakFilePath, int32_t pakOrder);
using HandleUnmountPakFn = bool (*)(void* self, const EngineString* pakFilePath);
using NativeThunkFn = void (*)(void* context, void* stack, void* result);

// After a mount, old assets only go away once nothing references them and a collection runs.
constexpr ULONGLONG kCollectWindowMs = 120000;
constexpr ULONGLONG kCollectIntervalMs = 5000;

struct MountJob {
    std::wstring enginePath;
    void* pakFile = nullptr;
    DWORD exceptionCode = 0;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~MountJob() { CloseHandle(done); }
};

Paths MakePaths() {
    // <Root>/<Project>/Binaries/Win64/HTGame.exe; the engine addresses content relative to that folder.
    std::wstring exe(1024, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
    fs::path projectDir = fs::path(exe).parent_path().parent_path().parent_path();
    Paths paths;
    paths.moduleDir = g_host->directory;
    paths.paksDir = projectDir / L"Content" / L"Paks";
    paths.enginePrefix = L"../../../" + projectDir.filename().wstring() + L"/Content/Paks/";
    return paths;
}

std::wstring Key(const fs::path& path) {
    std::wstring key = path.wstring();
    CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    return key;
}

std::vector<fs::path> ListPaks(const fs::path& directory) {
    std::vector<fs::path> paks;
    std::error_code error;
    fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        std::error_code ignored;
        if (it->is_regular_file(ignored) && _wcsicmp(it->path().extension().c_str(), L".pak") == 0) {
            paks.push_back(it->path());
        }
    }
    return paks;
}

// True when the pak and its IoStore companions are fully written; `totalSize` is their combined size.
bool ContainerComplete(const fs::path& pak, uint64_t& totalSize) {
    fs::path utoc = pak;
    fs::path ucas = pak;
    utoc.replace_extension(L".utoc");
    ucas.replace_extension(L".ucas");
    std::error_code error;
    bool hasUtoc = fs::exists(utoc, error);
    bool hasUcas = fs::exists(ucas, error);
    if (hasUtoc != hasUcas) {
        return false;
    }

    totalSize = 0;
    const fs::path* files[] = {&pak, &utoc, &ucas};
    for (const fs::path* file : files) {
        if (file != &pak && !hasUtoc) {
            continue;
        }
        // Fails with a sharing violation while the packager still has the file open for writing.
        HANDLE handle = CreateFileW(file->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        LARGE_INTEGER size{};
        GetFileSizeEx(handle, &size);
        CloseHandle(handle);
        if (size.QuadPart == 0) {
            return false;
        }
        totalSize += static_cast<uint64_t>(size.QuadPart);
    }
    return true;
}

void* CallHandleMountPak(const MountApi& api, const wchar_t* path, int32_t length, DWORD* exceptionCode) {
    EngineString text{path, length + 1, length + 1};
    __try {
        // INDEX_NONE lets the engine derive the order from the path exactly as it does at startup.
        return reinterpret_cast<HandleMountPakFn>(api.handleMountPak)(reinterpret_cast<void*>(api.pakPlatformFile),
                                                                       &text, -1);
    } __except (*exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool CallHandleUnmountPak(const MountApi& api, const wchar_t* path, int32_t length, DWORD* exceptionCode) {
    EngineString text{path, length + 1, length + 1};
    __try {
        return reinterpret_cast<HandleUnmountPakFn>(api.handleUnmountPak)(reinterpret_cast<void*>(api.pakPlatformFile), &text);
    } __except (*exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Whether someone holds the file open in a way that forbids deleting it, as the engine does with the
// files of a mounted container. Asks for delete access; nothing is deleted.
bool HeldOpen(const fs::path& file) {
    HANDLE handle = CreateFileW(file.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        return false;
    }
    return GetLastError() == ERROR_SHARING_VIOLATION;
}

// What the engine adds to the mount order of a pak because of its name, in hundreds: 0 without the
// _P suffix, N+1 for <name>_<N>_P, otherwise 1. Mirrors FPakPlatformFile::Mount.
int PatchRank(const fs::path& pak) {
    std::wstring name = pak.stem().wstring();
    if (name.size() < 2 || _wcsicmp(name.c_str() + name.size() - 2, L"_P") != 0) {
        return 0;
    }
    size_t versionEnd = name.size() - 2;
    size_t versionStart = versionEnd ? name.rfind(L'_', versionEnd - 1) : std::wstring::npos;
    if (versionStart == std::wstring::npos) {
        return 1;
    }
    std::wstring digits = name.substr(versionStart + 1, versionEnd - versionStart - 1);
    if (digits.empty() || digits.size() > 6 || digits.find_first_not_of(L"0123456789") != std::wstring::npos) {
        return 1;
    }
    int version = _wtoi(digits.c_str());
    return version >= 1 ? version + 1 : 1;
}

// ForceGarbageCollection only raises a flag; the engine collects on its next tick.
bool CallCollectGarbage(uintptr_t thunk) {
    // Stands in for the script frame. Its null code pointer makes the thunk's P_FINISH a no-op.
    alignas(16) uint8_t frame[0x200] = {};
    __try {
        reinterpret_cast<NativeThunkFn>(thunk)(nullptr, frame, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string ReadSmallFile(const fs::path& path) {
    std::string text;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        char buffer[8192];
        DWORD read = 0;
        if (ReadFile(file, buffer, sizeof buffer, &read, nullptr)) {
            text.assign(buffer, read);
        }
        CloseHandle(file);
    }
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) {
        text.erase(0, 3);
    }
    return text;
}

void WriteWholeFile(const fs::path& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        CloseHandle(file);
    }
}

class Watcher {
public:
    Watcher(Paths paths, MountApi api, uintptr_t collectGarbage, ObjectSystem* objects, Image* image)
        : paths_(std::move(paths)), api_(api), collectGarbage_(collectGarbage), objects_(objects), image_(image) {
        for (const fs::path& pak : ListPaks(paths_.paksDir)) {
            handled_.insert(Key(pak));
            mounted_[Key(pak)] = 0;
        }
        Log("磁盘上已有 %zu 个 pak，视为已挂载", handled_.size());
    }

    void Poll() {
        RunCommands();
        RequestCollection();
        UnmountWhenCollected();
        for (const fs::path& pak : ListPaks(paths_.paksDir)) {
            std::wstring key = Key(pak);
            if (handled_.count(key)) {
                continue;
            }
            uint64_t size = 0;
            if (!ContainerComplete(pak, size)) {
                pending_.erase(key);
                continue;
            }
            Pending& pending = pending_[key];
            if (pending.size != size) {
                pending = {size, 0};
                continue;
            }
            if (++pending.stablePolls < 2) {
                continue;
            }
            pending_.erase(key);
            handled_.insert(key);
            Mount(pak);
        }
    }

private:
    struct Pending {
        uint64_t size = 0;
        int stablePolls = 0;
    };

    void Mount(const fs::path& pak) {
        std::error_code error;
        fs::path relative = fs::relative(pak, paths_.paksDir, error);
        if (error || disabled_) {
            return;
        }
        auto job = std::make_shared<MountJob>();
        job->enginePath = paths_.enginePrefix + relative.generic_wstring();
        if (!pak.stem().wstring().ends_with(L"_P")) {
            Log("注意: %s 的名字不以 _P 结尾，优先级不会高于游戏本体", Utf8(pak.filename().wstring()).c_str());
        }
        Log("发现新包，请求挂载: %s", Utf8(job->enginePath).c_str());

        MountApi api = api_;
        if (!GameThread_Run([job, api] {
                job->pakFile = CallHandleMountPak(api, job->enginePath.c_str(),
                                                  static_cast<int32_t>(job->enginePath.size()), &job->exceptionCode);
                SetEvent(job->done);
            })) {
            Log("失败: 无法向游戏线程投递请求 (错误 %lu)", GetLastError());
            return;
        }

        const HANDLE handles[] = {job->done, g_stop};
        for (int waited = 0;; waited += 5) {
            DWORD result = WaitForMultipleObjects(2, handles, FALSE, 5000);
            if (result == WAIT_OBJECT_0) {
                break;
            }
            if (result != WAIT_TIMEOUT) {
                return;  // shutting down
            }
            if (waited == 25) {
                Log("游戏线程 30 秒内没有处理请求（可能正在加载），继续等待");
            }
        }

        if (job->exceptionCode) {
            disabled_ = true;
            Log("严重: 挂载调用抛出异常 %#lx，已停止后续挂载，建议重启游戏", job->exceptionCode);
            Beep(300, 600);
        } else if (job->pakFile) {
            Log("挂载成功 (IPakFile %p)", job->pakFile);
            mounted_[Key(pak)] = ++mountSequence_;
            Beep(1200, 120);
            Beep(1600, 160);
            ReloadMounted(pak);
        } else {
            Log("失败: 引擎拒绝挂载这个包（返回空）");
            Beep(400, 400);
        }
    }

    // Replaces what the game has loaded from the packages of a container that was just mounted, so
    // that the new version shows without a trip to the login screen.
    void ReloadMounted(const fs::path& pak) {
        fs::path utoc = pak;
        utoc.replace_extension(L".utoc");
        std::vector<uint64_t> ids = ReadContainerPackageIds(utoc.wstring());
        for (const Container& other : MountedModContainers()) {
            bool shares = std::find_first_of(ids.begin(), ids.end(), other.ids.begin(), other.ids.end()) != ids.end();
            if (shares && other.rank > PatchRank(pak)) {
                Log("注意: 已挂载的 %s 编号更大，同名资产游戏仍会用它。新版本请用比它更大的编号",
                    Utf8(other.pak.stem().wstring()).c_str());
                break;
            }
        }
        ReloadResult result = ReloadResult::Failed;
        std::string report;
        if (ids.empty()) {
            report = "读不出这个容器里有哪些包\r\n";
        } else if (!EnsureObjects()) {
            report = "对象系统不可用\r\n";
        } else {
            result = reloader_.Reload(ids, report);
        }
        WriteWholeFile(paths_.moduleDir / L"HTReload.reload.txt", report);
        LogReport("重载", report);
        ULONGLONG window = kCollectWindowMs;
        if (result == ReloadResult::Done) {
            Log("重载完成，新版本已经生效");
            Beep(2000, 200);
            window = 20000;  // the old assets are unreferenced now and go with the next collection
        } else if (result == ReloadResult::Failed) {
            Log("重载没有完成，明细在 HTReload.reload.txt。返回登录界面再进入可以让新版本生效");
            Beep(400, 400);
        }
        if (collectGarbage_ && !collectFailed_->load()) {
            collectUntil_ = GetTickCount64() + window;
            nextCollect_ = 0;
            Log("接下来 %llu 秒内每 %llu 秒请求一次垃圾回收", window / 1000, kCollectIntervalMs / 1000);
        }
        // With the game on the new version, the older ones can be unmounted once their assets are gone.
        if (result != ReloadResult::Failed) {
            unmountCheckAt_ = GetTickCount64() + 6000;
            unmountGiveUpAt_ = unmountCheckAt_ + 24000;
        }
    }

    // Writes the lines of a report to the log, leaving out the per-slot lines, which start with a bracket.
    static void LogReport(const char* prefix, const std::string& report) {
        std::istringstream lines(report);
        for (std::string line; std::getline(lines, line);) {
            while (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty() && line[0] != '[') {
                Log("%s: %s", prefix, line.c_str());
            }
        }
    }

    struct Container {
        fs::path pak;
        std::vector<uint64_t> ids;  // sorted
        int rank;                   // PatchRank
        int sequence;               // 0 if mounted when the game started, otherwise the order of mounting
    };

    // The mod containers that are mounted. A container counts as mounted while the plugin has it
    // down as such and its data file is held open; the second alone would also be true of a file
    // that the packager is still writing.
    std::vector<Container> MountedModContainers() {
        std::vector<Container> mounted;
        for (const fs::path& pak : ListPaks(paths_.paksDir)) {
            fs::path utoc = pak;
            fs::path ucas = pak;
            utoc.replace_extension(L".utoc");
            ucas.replace_extension(L".ucas");
            auto known = mounted_.find(Key(pak));
            if (pak.parent_path() == paths_.paksDir || known == mounted_.end() || !HeldOpen(ucas)) {
                continue;
            }
            std::vector<uint64_t> ids = ReadContainerPackageIds(utoc.wstring());
            if (!ids.empty()) {
                mounted.push_back({pak, std::move(ids), PatchRank(pak), known->second});
            }
        }
        return mounted;
    }

    // Mod containers that a newer version has made redundant: every package they hold is also in
    // another mounted mod container that has a higher patch number and was not mounted before them,
    // so the engine reads nothing from them any more. The second condition keeps a container that was
    // published with too low a number from being taken for an old version.
    std::vector<fs::path> ShadowedContainers() {
        std::vector<Container> mounted = MountedModContainers();
        std::vector<fs::path> shadowed;
        for (const Container& old : mounted) {
            for (const Container& newer : mounted) {
                if (newer.rank > old.rank && newer.sequence >= old.sequence &&
                    std::includes(newer.ids.begin(), newer.ids.end(), old.ids.begin(), old.ids.end())) {
                    shadowed.push_back(old.pak);
                    break;
                }
            }
        }
        return shadowed;
    }

    // Deletes the files of a container that the engine has let go of, and says how that went.
    static std::string DeleteContainer(const fs::path& pak) {
        // The data file goes first: should it refuse, the container is left complete.
        for (const wchar_t* extension : {L".ucas", L".utoc", L".pak"}) {
            fs::path file = pak;
            file.replace_extension(extension);
            DWORD error = DeleteFileW(file.c_str()) ? ERROR_SUCCESS : GetLastError();
            if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND) {
                return "已卸载，但删不掉 " + Utf8(file.filename().wstring()) + "（错误 " + std::to_string(error) + "）";
            }
        }
        return "已卸载并删除";
    }

    // Has the engine unmount the containers that newer versions have made redundant, which makes it
    // let go of their files, and deletes those files. Returns false when that has to wait because old
    // assets are still alive.
    bool UnmountShadowed(std::string& report) {
        if (!api_.handleUnmountPak && !unmountDisabled_) {
            // The engine binds this handler a moment after the mount handler, so the look at startup can be too early.
            image_->Refresh();
            api_.handleUnmountPak = FindUnmountHandler(*image_, api_.pakPlatformFile);
        }
        if (!api_.handleUnmountPak || unmountDisabled_) {
            report += unmountDisabled_ ? "卸载已停用\r\n" : "没找到引擎的卸载入口\r\n";
            return true;
        }
        std::vector<fs::path> shadowed = ShadowedContainers();
        if (shadowed.empty()) {
            report += "没有被新版本完全取代的旧容器\r\n";
            return true;
        }
        if (!EnsureObjects()) {
            report += "对象系统不可用，不卸载\r\n";
            return true;
        }
        size_t retired = reloader_.RetiredPackagesAlive();
        if (retired) {
            report += "还有 " + std::to_string(retired) + " 个旧包没有被回收，旧容器保持挂载\r\n";
            return false;
        }
        for (const fs::path& pak : shadowed) {
            std::error_code error;
            fs::path relative = fs::relative(pak, paths_.paksDir, error);
            if (error || unmountDisabled_) {
                continue;
            }
            std::wstring enginePath = paths_.enginePrefix + relative.generic_wstring();
            fs::path ucas = pak;
            ucas.replace_extension(L".ucas");
            bool unmounted = false;
            DWORD exceptionCode = 0;
            MountApi api = api_;
            bool ran = GameThread_RunAndWait(
                [&] {
                    unmounted = CallHandleUnmountPak(api, enginePath.c_str(), static_cast<int32_t>(enginePath.size()), &exceptionCode);
                },
                30000);
            std::string name = Utf8(pak.stem().wstring());
            if (!ran) {
                report += name + ": 游戏线程没有执行卸载\r\n";
            } else if (exceptionCode) {
                unmountDisabled_ = true;
                char code[32];
                snprintf(code, sizeof code, "%#lx", exceptionCode);
                report += name + ": 严重: 卸载调用抛出异常 " + code + "，已停止后续卸载，建议重启游戏\r\n";
                Beep(300, 600);
            } else if (!unmounted) {
                report += name + ": 引擎没有卸载它（返回否），路径 " + Utf8(enginePath) + "\r\n";
            } else if (HeldOpen(ucas)) {
                mounted_.erase(Key(pak));
                report += name + ": 引擎已卸载，但文件仍被占用，没有删除\r\n";
            } else {
                mounted_.erase(Key(pak));
                report += name + ": " + DeleteContainer(pak) + "\r\n";
            }
        }
        return true;
    }

    // After a reload the old assets take a collection or two to go away; the old containers are
    // unmounted once they have.
    void UnmountWhenCollected() {
        ULONGLONG now = GetTickCount64();
        if (!unmountCheckAt_ || now < unmountCheckAt_) {
            return;
        }
        std::string report;
        if (!UnmountShadowed(report) && now < unmountGiveUpAt_) {
            unmountCheckAt_ = now + 5000;
            return;
        }
        unmountCheckAt_ = 0;
        LogReport("卸载", report);
    }

    // Instructions dropped into HTReload.cmd.txt next to the plugin, one per line. Answers go to
    // HTReload.out.txt. Besides the read-only queries in explore.h:
    //   gc [seconds]            request collections for that long (default 15)
    //   probe <label> [filter]  report on the packages of the mod containers whose path contains `filter`
    //   engine                  locate the engine functions a reload calls
    //   calltest                call a harmless script function through the engine
    //   plan [filter]           list what a reload of those containers would do
    //   renameload <asset>      WRITES: rename the package holding that asset and load the asset again
    //   reload [filter]         WRITES: replace the loaded assets of those containers by fresh copies
    //   shadowed                list the mounted mod containers that a newer version has made redundant
    //   unmount                 WRITES: have the engine unmount those containers and DELETE their files
    void RunCommands() {
        fs::path path = paths_.moduleDir / L"HTReload.cmd.txt";
        std::error_code error;
        if (!fs::exists(path, error)) {
            return;
        }
        std::string text = ReadSmallFile(path);
        if (!DeleteFileW(path.c_str())) {
            return;  // still being written; pick it up on the next poll
        }
        std::string answers;
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            size_t space = line.find(' ');
            std::string command = line.substr(0, space);
            std::string arguments = space == std::string::npos ? std::string() : line.substr(space + 1);
            if (command.empty()) {
                continue;
            }
            Log("指令: %s", line.c_str());
            answers += "### " + line + "\r\n";
            try {
                RunCommand(command, arguments, answers);
            } catch (const std::exception& exception) {
                Log("指令失败: %s", exception.what());
                answers += std::string("失败: ") + exception.what() + "\r\n";
            }
            Log("指令完成: %s", line.c_str());
        }
        WriteWholeFile(paths_.moduleDir / L"HTReload.out.txt", answers);
    }

    void RunCommand(const std::string& command, const std::string& arguments, std::string& answers) {
        std::istringstream words(arguments);
        std::string first;
        std::string second;
        words >> first >> second;
        if (command == "gc") {
            if (collectGarbage_ && !collectFailed_->load()) {
                int seconds = first.empty() ? 15 : std::clamp(atoi(first.c_str()), 1, 600);
                collectUntil_ = GetTickCount64() + seconds * 1000ull;
                nextCollect_ = 0;
            }
            return;
        }
        if (!EnsureObjects()) {
            answers += "对象系统不可用\r\n";
            return;
        }
        if (command == "probe") {
            std::vector<uint64_t> ids = ModPackageIds(second);
            if (ids.empty()) {
                answers += "没有匹配的 Mod 容器\r\n";
            } else {
                ProbeSupersededPackages(*objects_, ids, reportPath_, first.empty() ? "手动" : first.c_str());
            }
        } else if (command == "engine") {
            reloader_.Resolve(answers);
        } else if (command == "calltest") {
            reloader_.CallTest(answers);
        } else if (command == "plan") {
            reloader_.Plan(ModPackageIds(first), answers);
        } else if (command == "renameload") {
            reloader_.RenameAndLoad(first, answers);
        } else if (command == "reload") {
            reloader_.Reload(ModPackageIds(first), answers);
            // The old assets are unreferenced now; let the engine take them away.
            if (collectGarbage_ && !collectFailed_->load()) {
                collectUntil_ = GetTickCount64() + 20000;
                nextCollect_ = 0;
            }
        } else if (command == "shadowed") {
            for (const fs::path& pak : ShadowedContainers()) {
                answers += Utf8(pak.wstring()) + "\r\n";
            }
            answers += "还活着的旧包: " + std::to_string(reloader_.RetiredPackagesAlive()) + " 个\r\n";
        } else if (command == "unmount") {
            UnmountShadowed(answers);
        } else if (!Explore(command, arguments, *image_, *objects_, answers)) {
            answers += "不认识的指令\r\n";
        }
    }

    // Package ids of every mod container on disk, i.e. the containers below the game's own Paks folder.
    std::vector<uint64_t> ModPackageIds(const std::string& filter) {
        std::string wanted = filter;
        CharLowerBuffA(wanted.data(), static_cast<DWORD>(wanted.size()));
        std::vector<uint64_t> ids;
        for (const fs::path& pak : ListPaks(paths_.paksDir)) {
            if (pak.parent_path() == paths_.paksDir || Utf8(Key(pak)).find(wanted) == std::string::npos) {
                continue;
            }
            fs::path utoc = pak;
            utoc.replace_extension(L".utoc");
            std::vector<uint64_t> container = ReadContainerPackageIds(utoc.wstring());
            ids.insert(ids.end(), container.begin(), container.end());
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        return ids;
    }

    // The object table only appears once the engine is up, so it is looked for when first needed.
    bool EnsureObjects() {
        image_->Refresh();
        if (!objects_->Ready()) {
            objects_->Locate(*image_);
        }
        return objects_->Ready();
    }

    void RequestCollection() {
        ULONGLONG now = GetTickCount64();
        if (!collectUntil_) {
            return;
        }
        if (collectFailed_->load()) {
            Log("严重: 请求垃圾回收时抛出异常，已停用这个功能");
            collectUntil_ = 0;
        } else if (now >= collectUntil_) {
            Log("垃圾回收请求结束");
            collectUntil_ = 0;
        } else if (now >= nextCollect_) {
            nextCollect_ = now + kCollectIntervalMs;
            GameThread_Run([thunk = collectGarbage_, failed = collectFailed_] {
                if (!CallCollectGarbage(thunk)) {
                    failed->store(true);
                }
            });
        }
    }

    Paths paths_;
    MountApi api_;
    uintptr_t collectGarbage_;
    ObjectSystem* objects_;
    Image* image_;
    Reloader reloader_{*image_, *objects_};
    std::wstring reportPath_ = (paths_.moduleDir / L"HTReload.probe.txt").wstring();
    std::set<std::wstring> handled_;
    std::map<std::wstring, int> mounted_;  // paks the engine has mounted, as far as the plugin knows, with Container::sequence
    int mountSequence_ = 0;
    std::map<std::wstring, Pending> pending_;
    bool disabled_ = false;
    bool unmountDisabled_ = false;
    ULONGLONG unmountCheckAt_ = 0;  // when to look next whether old containers can be unmounted; 0 = not pending
    ULONGLONG unmountGiveUpAt_ = 0;
    ULONGLONG collectUntil_ = 0;
    ULONGLONG nextCollect_ = 0;
    std::shared_ptr<std::atomic<bool>> collectFailed_ = std::make_shared<std::atomic<bool>>(false);
};

void Run() {
    Paths paths = MakePaths();
    Log("核心 %s，监视目录: %s", kVersion, Utf8(paths.paksDir.wstring()).c_str());

    Image image;
    if (!image.Open()) {
        Log("失败: 无法读取游戏主模块");
        return;
    }

    // At game start the plugin loads before the game's own entry point runs; give the unpacker a moment.
    if (g_generation == 0 && WaitStop(3000)) {
        return;
    }

    Resolver resolver;
    for (int attempt = 1;; ++attempt) {
        image.Refresh();
        if (resolver.Step(image)) {
            break;
        }
        if (attempt >= 300) {
            Log("放弃: 一直没能定位挂载入口，本次运行不提供热重载");
            return;
        }
        if (WaitStop(attempt < 15 ? 1000 : 5000)) {
            return;
        }
    }

    // The delegate is bound right after the engine's startup scan, so everything on disk now is mounted.
    uintptr_t collectGarbage = FindCollectGarbageThunk(image);
    MountApi api = resolver.Api();
    api.handleUnmountPak = FindUnmountHandler(image, api.pakPlatformFile);
    ObjectSystem objects;
    Watcher watcher(paths, api, collectGarbage, &objects, &image);

    while (!GameThread_Ready()) {
        if (WaitStop(1000)) {
            return;
        }
    }
    Log("就绪: 开始监视新包和指令");

    while (!WaitStop(1000)) {
        watcher.Poll();
    }
}

DWORD WINAPI Worker(void*) {
    try {
        Run();
    } catch (const std::exception& exception) {
        Log("异常退出: %s", exception.what());
    } catch (...) {
        Log("异常退出");
    }
    return 0;
}

}  // namespace

extern "C" __declspec(dllexport) bool CoreStart(const CoreHost* host) {
    if (!host || host->abi != kCoreHostAbi) {
        return false;
    }
    g_host = host;
    g_generation = host->generation;
    LogAttach(host);
    GameThread_Attach(host);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    return g_worker != nullptr;
}

extern "C" __declspec(dllexport) bool CoreStop() {
    SetEvent(g_stop);
    bool stopped = WaitForSingleObject(g_worker, 60000) == WAIT_OBJECT_0;
    if (stopped) {
        GameThread_Clear();
        CloseHandle(g_worker);
        CloseHandle(g_stop);
    }
    return stopped;
}
