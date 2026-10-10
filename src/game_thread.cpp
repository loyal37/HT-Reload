#include "game_thread.h"

#include <windows.h>

#include <memory>
#include <mutex>
#include <vector>

#include "host.h"

namespace {

const CoreHost* g_host = nullptr;
std::mutex g_mutex;
std::vector<std::function<void()>> g_tasks;

void Drain(void*) {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard lock(g_mutex);
        tasks.swap(g_tasks);
    }
    for (std::function<void()>& task : tasks) {
        task();
    }
}

}  // namespace

void GameThread_Attach(const CoreHost* host) {
    g_host = host;
}

bool GameThread_Ready() {
    return g_host && g_host->gameThreadReady();
}

bool GameThread_Run(std::function<void()> task) {
    {
        std::lock_guard lock(g_mutex);
        g_tasks.push_back(std::move(task));
    }
    return g_host && g_host->runOnGameThread(Drain, nullptr);
}

bool GameThread_RunAndWait(std::function<void()> task, unsigned long timeoutMs) {
    struct State {
        std::mutex mutex;
        bool cancelled = false;
        bool done = false;
        HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~State() { CloseHandle(finished); }
    };
    auto state = std::make_shared<State>();
    bool queued = GameThread_Run([state, task = std::move(task)] {
        std::lock_guard lock(state->mutex);
        if (!state->cancelled) {
            task();
            state->done = true;
        }
        SetEvent(state->finished);
    });
    if (queued) {
        WaitForSingleObject(state->finished, timeoutMs);
    }
    // Taking the lock waits out a task that is running, and the flag stops one that has not started,
    // so the caller's locals are never touched after this returns.
    std::lock_guard lock(state->mutex);
    state->cancelled = true;
    return state->done;
}

void GameThread_Clear() {
    std::lock_guard lock(g_mutex);
    g_tasks.clear();
}
