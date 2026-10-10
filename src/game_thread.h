#pragma once

#include <functional>

struct CoreHost;

// Work on the game thread goes through the shell, which owns the hook into the game's message loop.
void GameThread_Attach(const CoreHost* host);

// False until the game window exists.
bool GameThread_Ready();

// Queues `task` for the game thread.
bool GameThread_Run(std::function<void()> task);

// Runs `task` on the game thread and waits for it. Returns false if it did not run within the
// timeout, in which case it never will; a task that has started is always waited for.
bool GameThread_RunAndWait(std::function<void()> task, unsigned long timeoutMs);

// Drops tasks that have not run yet. Called while the core shuts down.
void GameThread_Clear();
