#pragma once

#include <functional>

// Runs work on the game thread, which is the thread that owns the Unreal window and pumps its
// messages once per frame outside of world ticking.

// False until the game window exists.
bool GameThread_Attach();

// Queues `task` for the game thread.
bool GameThread_Run(std::function<void()> task);
