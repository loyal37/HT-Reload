#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "image.h"
#include "objects.h"

// Engine entry points that the reload needs to call, located in the running game.
struct EngineCalls {
    size_t renameSlot = 0;           // UObject::Rename in the virtual table
    size_t processEventSlot = 0;     // UObject::ProcessEvent in the virtual table
    size_t functionFlagsOffset = 0;  // UFunction::FunctionFlags; NumParms, ParmsSize and ReturnValueOffset follow it

    bool Ready() const { return renameSlot && processEventSlot && functionFlagsOffset; }

    // Appends what it finds to `report`.
    bool Resolve(const Image& image, const ObjectSystem& objects, std::string& report);

    // Size of the parameter block of a UFunction object.
    uint16_t ParamsSize(uintptr_t function) const;
};

// The UFunction `functionName` of the class `className`, or 0.
uintptr_t FindFunction(const ObjectSystem& system, const ArenaVector<ObjectInfo>& objects, const char* className,
                       const char* functionName);

// Both must run on the game thread. They return false and set `exceptionCode` if the engine faults.
bool CallProcessEvent(const EngineCalls& calls, uintptr_t object, uintptr_t function, void* params, DWORD* exceptionCode);
bool CallRename(const EngineCalls& calls, uintptr_t object, const wchar_t* newName, bool* renamed, DWORD* exceptionCode);
