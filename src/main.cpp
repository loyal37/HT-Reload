// HTReload core: mounts pak mods that appear under <Project>/Content/Paks while the game is running,
// by calling the engine's own FCoreDelegates::MountPak handler on the game thread. Loaded and
// replaced at run time by the shell (see host.h).

#include <windows.h>

#include <algorithm>
#include <atomic>
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
#include "resolver.h"

namespace fs = std::filesystem;

namespace {

const char kVersion[] = "0.4.0";

const CoreHost* g_host = nullptr;
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
        }
        Log("磁盘上已有 %zu 个 pak，视为已挂载", handled_.size());
    }

    void Poll() {
        RunCommands();
        RequestCollection();
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
            Log("挂载成功 (IPakFile %p)。返回登录界面再进入后生效", job->pakFile);
            if (collectGarbage_ && !collectFailed_->load()) {
                collectUntil_ = GetTickCount64() + kCollectWindowMs;
                nextCollect_ = 0;
                Log("接下来 %llu 秒内每 %llu 秒请求一次垃圾回收", kCollectWindowMs / 1000, kCollectIntervalMs / 1000);
            }
            Beep(1200, 120);
            Beep(1600, 160);
        } else {
            Log("失败: 引擎拒绝挂载这个包（返回空）");
            Beep(400, 400);
        }
    }

    // Instructions dropped into HTReload.cmd.txt next to the plugin, one per line. Answers go to
    // HTReload.out.txt. Besides the read-only queries in explore.h:
    //   gc [seconds]            request collections for that long (default 15)
    //   probe <label> [filter]  report on the packages of the mod containers whose path contains `filter`
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
    std::wstring reportPath_ = (paths_.moduleDir / L"HTReload.probe.txt").wstring();
    std::set<std::wstring> handled_;
    std::map<std::wstring, Pending> pending_;
    bool disabled_ = false;
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
    if (g_host->generation == 0 && WaitStop(3000)) {
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
    ObjectSystem objects;
    Watcher watcher(paths, resolver.Api(), collectGarbage, &objects, &image);

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
