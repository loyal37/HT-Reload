#pragma once

#include <functional>

struct CoreHost;

// Work on the game thread goes through the shell, which owns the hook into the game's message loop.
void GameThread_Attach(const CoreHost* host);

// False until the game window exists.
bool GameThread_Ready();

// Queues `task` for the game thread.
bool GameThread_Run(std::function<void()> task);

// Drops tasks that have not run yet. Called while the core shuts down.
void GameThread_Clear();
