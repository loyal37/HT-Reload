#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "objects.h"

// Package ids (FPackageId) of every package stored in an IoStore container, sorted.
std::vector<uint64_t> ReadContainerPackageIds(const std::wstring& utocPath);

// FPackageId::FromName.
uint64_t PackageIdFromName(const std::string& name);

// A pointer to a superseded object, stored in the body of an object that is not itself superseded.
struct ReferenceSlot {
    uintptr_t location;
    int32_t owner;   // index into the view
    int32_t target;  // index into the view
};

// What a mod container supersedes among the loaded objects.
struct Superseded {
    std::vector<int32_t> packages;     // loaded packages the container also holds
    std::vector<int32_t> objects;      // everything inside those packages, the packages included
    std::vector<ReferenceSlot> slots;  // filled when `findSlots` is set
};
Superseded FindSuperseded(const ObjectView& view, const std::vector<uint64_t>& packageIds, bool findSlots);

// Lists every engine-owned slot that holds `target`: the object it lies in, or the surrounding memory.
void ReportReferences(const ObjectSystem& objects, uintptr_t target, std::string& out);

// Appends a report to `reportPath` about the loaded packages a container supersedes: which of their
// objects are alive and every place in the process that still points at them.
void ProbeSupersededPackages(const ObjectSystem& objects, const std::vector<uint64_t>& packageIds,
                             const std::wstring& reportPath, const char* label);
