#pragma once

#include <cstdint>
#include <vector>

#include "image.h"

// The engine's own runtime pak mounting, located in the running game.
struct MountApi {
    uintptr_t handleMountPak = 0;   // IPakFile* FPakPlatformFile::HandleMountPakDelegate(const FString&, int32)
    uintptr_t pakPlatformFile = 0;  // the FPakPlatformFile bound to FCoreDelegates::MountPak
};

// Address of UKismetSystemLibrary::execCollectGarbage, the script thunk that asks the engine for a
// full garbage collection on its next tick. 0 when it cannot be identified.
uintptr_t FindCollectGarbageThunk(const Image& image);

class Resolver {
public:
    // Returns true once the API is known. Meant to be retried while the game starts up.
    bool Step(const Image& image);
    const MountApi& Api() const { return api_; }

private:
    bool FindCandidates(const Image& image, bool verbose);
    bool FindBoundInstance(const Image& image, bool verbose);

    std::vector<uintptr_t> candidates_;
    MountApi api_;
    int attempts_ = 0;
};
