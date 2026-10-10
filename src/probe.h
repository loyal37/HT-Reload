#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "objects.h"

// Package ids (FPackageId) of every package stored in an IoStore container, sorted.
std::vector<uint64_t> ReadContainerPackageIds(const std::wstring& utocPath);

// FPackageId::FromName.
uint64_t PackageIdFromName(const std::string& name);

// Lists every engine-owned slot that holds `target`: the object it lies in, or the surrounding memory.
void ReportReferences(const ObjectSystem& objects, uintptr_t target, std::string& out);

// Appends a report to `reportPath` about the loaded packages a container supersedes: which of their
// objects are alive and every place in the process that still points at them.
void ProbeSupersededPackages(const ObjectSystem& objects, const std::vector<uint64_t>& packageIds,
                             const std::wstring& reportPath, const char* label);
