#pragma once

#include <cstddef>
#include <cstdint>

// CityHash64 v1.1, the hash the engine uses to turn a package name into its FPackageId.
uint64_t CityHash64(const char* data, size_t size);
