#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct CoreHost;

// The log file belongs to the shell; the core writes through it.
void LogAttach(const CoreHost* host);
void Log(const char* format, ...);

std::string Utf8(const std::wstring& text);
std::string Hex(const uint8_t* bytes, size_t size);
