#include "game_thread.h"

#include <windows.h>

#include <cwchar>
#include <mutex>
#include <vector>

namespace {

std::mutex g_mutex;
std::vector<std::function<void()>> g_tasks;
DWORD g_threadId = 0;
HHOOK g_hook = nullptr;
UINT g_message = 0;

void RunTasks() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard lock(g_mutex);
        tasks.swap(g_tasks);
    }
    for (std::function<void()>& task : tasks) {
        task();
    }
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

}  // namespace

bool GameThread_Attach() {
    if (g_hook) {
        return true;
    }
    DWORD threadId = 0;
    EnumWindows(FindUnrealWindow, reinterpret_cast<LPARAM>(&threadId));
    if (!threadId) {
        return false;
    }
    g_message = RegisterWindowMessageW(L"HTReload.RunTasks");
    g_hook = SetWindowsHookExW(WH_GETMESSAGE, OnGetMessage, nullptr, threadId);
    if (!g_hook) {
        return false;
    }
    g_threadId = threadId;
    return true;
}

bool GameThread_Run(std::function<void()> task) {
    {
        std::lock_guard lock(g_mutex);
        g_tasks.push_back(std::move(task));
    }
    return PostThreadMessageW(g_threadId, g_message, 0, 0) != 0;
}
