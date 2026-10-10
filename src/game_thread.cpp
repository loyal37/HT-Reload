#include "game_thread.h"

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

void GameThread_Clear() {
    std::lock_guard lock(g_mutex);
    g_tasks.clear();
}
