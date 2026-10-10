#pragma once

#include <cstdint>

// Contract between the shell (HTReload.asi, loaded once by the ASI loader and never unloaded) and
// the core (HTReload.core.dll, which the shell loads and replaces while the game keeps running).

constexpr uint32_t kCoreHostAbi = 1;

struct CoreHost {
    uint32_t abi;
    // Number of cores loaded before this one in the current process; 0 right after game start.
    uint32_t generation;
    // Folder holding the plugin, its log, command and report files.
    const wchar_t* directory;
    // Appends one UTF-8 line to the log.
    void (*log)(const char* line);
    // False until the game window exists.
    bool (*gameThreadReady)();
    // Queues `task(context)` for the game thread. The shell guarantees that no task is running or
    // will run once CoreStop has returned.
    bool (*runOnGameThread)(void (*task)(void* context), void* context);
};

// Exported by the core.
extern "C" {
using CoreStartFn = bool (*)(const CoreHost* host);
// Returns once every thread of the core has ended; false if that did not happen in time.
using CoreStopFn = bool (*)();
}
