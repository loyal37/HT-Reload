// HTReload: mounts pak mods that appear under <Project>/Content/Paks while the game is running,
// by calling the engine's own FCoreDelegates::MountPak handler on the game thread.

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "game_thread.h"
#include "image.h"
#include "log.h"
#include "resolver.h"

namespace fs = std::filesystem;

namespace {

const char kVersion[] = "0.2.0";

HMODULE g_module = nullptr;

struct Paths {
    fs::path moduleDir;         // where this .asi lives
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

fs::path ModulePath(HMODULE module) {
    std::wstring buffer(1024, L'\0');
    DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return buffer;
}

bool IsGameProcess() {
    return _wcsicmp(ModulePath(nullptr).filename().c_str(), L"HTGame.exe") == 0;
}

Paths MakePaths() {
    // <Root>/<Project>/Binaries/Win64/HTGame.exe; the engine addresses content relative to that folder.
    fs::path projectDir = ModulePath(nullptr).parent_path().parent_path().parent_path();
    Paths paths;
    paths.moduleDir = ModulePath(g_module).parent_path();
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

class Watcher {
public:
    Watcher(Paths paths, MountApi api, uintptr_t collectGarbage)
        : paths_(std::move(paths)), api_(api), collectGarbage_(collectGarbage) {
        for (const fs::path& pak : ListPaks(paths_.paksDir)) {
            handled_.insert(Key(pak));
        }
        Log("启动时已有 %zu 个 pak，视为引擎已挂载", handled_.size());
    }

    void Poll() {
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

        for (int waited = 0; WaitForSingleObject(job->done, 5000) == WAIT_TIMEOUT; waited += 5) {
            if (waited == 25) {
                Log("游戏线程 30 秒内没有处理请求（可能正在加载），继续等待");
            }
        }

        if (job->exceptionCode) {
            disabled_ = true;
            Log("严重: 挂载调用抛出异常 %#lx，已停止后续挂载，建议重启游戏", job->exceptionCode);
            Beep(300, 600);
        } else if (job->pakFile) {
            Log("挂载成功 (IPakFile %p)。让游戏卸载再加载这个角色后生效", job->pakFile);
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
    std::set<std::wstring> handled_;
    std::map<std::wstring, Pending> pending_;
    bool disabled_ = false;
    ULONGLONG collectUntil_ = 0;
    ULONGLONG nextCollect_ = 0;
    std::shared_ptr<std::atomic<bool>> collectFailed_ = std::make_shared<std::atomic<bool>>(false);
};

void Run() {
    Paths paths = MakePaths();
    // Keep the previous run's log: it is the only record left if that run ended in a crash.
    fs::path logPath = paths.moduleDir / L"HTReload.log";
    MoveFileExW(logPath.c_str(), (paths.moduleDir / L"HTReload.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    LogOpen(logPath.wstring());
    Log("HTReload %s，监视目录: %s", kVersion, Utf8(paths.paksDir.wstring()).c_str());

    Image image;
    if (!image.Open()) {
        Log("失败: 无法读取游戏主模块");
        return;
    }
    Log("主模块 %#zx 大小 %#zx", image.base, image.end - image.base);

    // The plugin loads before the game's own entry point runs; give the unpacker a moment first.
    Sleep(3000);

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
        Sleep(attempt < 15 ? 1000 : 5000);
    }

    // The delegate is bound right after the engine's startup scan, so everything on disk now is mounted.
    Watcher watcher(paths, resolver.Api(), FindCollectGarbageThunk(image));

    while (!GameThread_Attach()) {
        Sleep(1000);
    }
    Log("就绪: 已接入游戏线程，开始监视新包");

    for (;;) {
        Sleep(1000);
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

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        if (IsGameProcess()) {
            CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr));
        }
    }
    return TRUE;
}
