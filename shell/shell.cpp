// HTReload shell: the small part that the ASI loader loads and that stays for the life of the game.
// It owns the log, runs work on the game thread, and loads HTReload.core.dll, swapping in a new
// build whenever that file changes so the core can be developed without restarting the game.

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "host.h"

namespace fs = std::filesystem;

namespace {

HMODULE g_module = nullptr;
std::wstring g_directory;

// --- log ------------------------------------------------------------------------------------

HANDLE g_log = INVALID_HANDLE_VALUE;
SRWLOCK g_logLock = SRWLOCK_INIT;

void LogLine(const char* text) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[2200];
    int length = snprintf(line, sizeof line, "[%02u:%02u:%02u.%03u] %.2100s\r\n", now.wHour, now.wMinute, now.wSecond,
                          now.wMilliseconds, text);
    AcquireSRWLockExclusive(&g_logLock);
    if (g_log != INVALID_HANDLE_VALUE && length > 0) {
        DWORD written = 0;
        WriteFile(g_log, line, static_cast<DWORD>(length), &written, nullptr);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

void Logf(const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    LogLine(text);
}

void OpenLog() {
    // Keep the previous run's log: it is the only record left if that run ended in a crash.
    std::wstring path = g_directory + L"\\HTReload.log";
    MoveFileExW(path.c_str(), (g_directory + L"\\HTReload.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_log = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_log, "\xEF\xBB\xBF", 3, &written, nullptr);
    }
}

// --- game thread ----------------------------------------------------------------------------

struct Task {
    void (*run)(void*);
    void* context;
};

// Held shared while core code runs on the game thread and exclusive while the core is swapped.
SRWLOCK g_coreLock = SRWLOCK_INIT;
std::mutex g_taskMutex;
std::vector<Task> g_tasks;
HHOOK g_hook = nullptr;
DWORD g_gameThread = 0;
UINT g_message = 0;
bool g_running = false;  // game thread only

void RunTasks() {
    if (g_running) {
        return;  // a task is pumping messages itself; the outer call picks up the rest
    }
    g_running = true;
    AcquireSRWLockShared(&g_coreLock);
    std::vector<Task> tasks;
    {
        std::lock_guard lock(g_taskMutex);
        tasks.swap(g_tasks);
    }
    for (const Task& task : tasks) {
        task.run(task.context);
    }
    ReleaseSRWLockShared(&g_coreLock);
    g_running = false;
}

LRESULT CALLBACK OnGetMessage(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == PM_REMOVE) {
        const MSG* message = reinterpret_cast<const MSG*>(lParam);
        if (message->hwnd == nullptr && message->message == g_message) {
            RunTasks();
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL CALLBACK FindUnrealWindow(HWND window, LPARAM result) {
    DWORD processId = 0;
    DWORD threadId = GetWindowThreadProcessId(window, &processId);
    wchar_t className[64]{};
    if (processId != GetCurrentProcessId() || !GetClassNameW(window, className, 64) ||
        wcscmp(className, L"UnrealWindow") != 0) {
        return TRUE;
    }
    *reinterpret_cast<DWORD*>(result) = threadId;
    return FALSE;
}

// The game thread owns the Unreal window and pumps its messages once per frame, outside of world ticking.
void AttachToGameThread() {
    if (g_hook) {
        return;
    }
    DWORD threadId = 0;
    EnumWindows(FindUnrealWindow, reinterpret_cast<LPARAM>(&threadId));
    if (!threadId) {
        return;
    }
    g_message = RegisterWindowMessageW(L"HTReload.RunTasks");
    g_gameThread = threadId;
    g_hook = SetWindowsHookExW(WH_GETMESSAGE, OnGetMessage, nullptr, threadId);
    if (g_hook) {
        Logf("外壳: 已接入游戏线程 %lu", threadId);
    }
}

bool HostGameThreadReady() {
    return g_hook != nullptr;
}

bool HostRunOnGameThread(void (*task)(void*), void* context) {
    if (!g_hook) {
        return false;
    }
    {
        std::lock_guard lock(g_taskMutex);
        g_tasks.push_back({task, context});
    }
    return PostThreadMessageW(g_gameThread, g_message, 0, 0) != 0;
}

// --- core -----------------------------------------------------------------------------------

struct FileStamp {
    FILETIME written{};
    ULONGLONG size = 0;
    bool exists = false;

    bool operator==(const FileStamp& other) const {
        return exists == other.exists && size == other.size && CompareFileTime(&written, &other.written) == 0;
    }
};

FileStamp StampOf(const std::wstring& path) {
    FileStamp stamp;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        stamp.exists = true;
        stamp.written = data.ftLastWriteTime;
        stamp.size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }
    return stamp;
}

struct Core {
    HMODULE module = nullptr;
    CoreStopFn stop = nullptr;
    std::wstring copy;
    FileStamp stamp;
};

Core g_core;
CoreHost g_host{};

void UnloadCore() {
    if (!g_core.module) {
        return;
    }
    bool stopped = g_core.stop ? g_core.stop() : true;
    // After this no core task is running on the game thread and none is left queued.
    AcquireSRWLockExclusive(&g_coreLock);
    {
        std::lock_guard lock(g_taskMutex);
        g_tasks.clear();
    }
    ReleaseSRWLockExclusive(&g_coreLock);
    if (stopped) {
        FreeLibrary(g_core.module);
        DeleteFileW(g_core.copy.c_str());
    } else {
        Logf("外壳: 旧核心没能在限定时间内停下，保留在内存里不卸载");
    }
    g_core = Core{};
}

void LoadCore(const std::wstring& source, const FileStamp& stamp) {
    // Run a copy so that the file itself can be replaced while the game is running.
    Core core;
    core.stamp = stamp;
    core.copy = g_directory + L"\\HTReload.core.run" + std::to_wstring(g_host.generation) + L".dll";
    if (!CopyFileW(source.c_str(), core.copy.c_str(), FALSE)) {
        Logf("外壳: 复制核心失败 (错误 %lu)", GetLastError());
        return;
    }
    core.module = LoadLibraryW(core.copy.c_str());
    if (!core.module) {
        Logf("外壳: 加载核心失败 (错误 %lu)", GetLastError());
        return;
    }
    auto start = reinterpret_cast<CoreStartFn>(GetProcAddress(core.module, "CoreStart"));
    core.stop = reinterpret_cast<CoreStopFn>(GetProcAddress(core.module, "CoreStop"));
    if (!start || !core.stop || !start(&g_host)) {
        Logf("外壳: 核心拒绝启动或缺少入口");
        FreeLibrary(core.module);
        DeleteFileW(core.copy.c_str());
        return;
    }
    Logf(g_host.generation == 0 ? "外壳: 核心已加载" : "外壳: 核心已更换（第 %u 次）", g_host.generation);
    ++g_host.generation;
    g_core = core;
}

DWORD WINAPI Run(void*) {
    OpenLog();
    Logf("外壳: 启动，目录 %ls", g_directory.c_str());

    // Copies left behind by an earlier run of the game.
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(g_directory, error)) {
        if (entry.path().filename().wstring().rfind(L"HTReload.core.run", 0) == 0) {
            DeleteFileW(entry.path().c_str());
        }
    }

    g_host.abi = kCoreHostAbi;
    g_host.directory = g_directory.c_str();
    g_host.log = LogLine;
    g_host.gameThreadReady = HostGameThreadReady;
    g_host.runOnGameThread = HostRunOnGameThread;

    std::wstring source = g_directory + L"\\HTReload.core.dll";
    FileStamp seen;
    for (;;) {
        AttachToGameThread();
        // Swap once the file has stopped changing, so a build in progress is never loaded.
        FileStamp stamp = StampOf(source);
        if (stamp.exists && stamp == seen && !(stamp == g_core.stamp)) {
            UnloadCore();
            LoadCore(source, stamp);
            if (!g_core.module) {
                g_core.stamp = stamp;  // do not retry the same broken file every second
            }
        }
        seen = stamp;
        Sleep(1000);
    }
}

bool IsGameProcess() {
    wchar_t path[1024];
    DWORD length = GetModuleFileNameW(nullptr, path, 1024);
    const wchar_t* name = wcsrchr(path, L'\\');
    return length > 0 && _wcsicmp(name ? name + 1 : path, L"HTGame.exe") == 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        if (IsGameProcess()) {
            wchar_t path[1024];
            GetModuleFileNameW(module, path, 1024);
            g_directory = fs::path(path).parent_path().wstring();
            CloseHandle(CreateThread(nullptr, 0, Run, nullptr, 0, nullptr));
        }
    }
    return TRUE;
}
