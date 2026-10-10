#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

void LogOpen(const std::wstring& path);
void Log(const char* format, ...);

std::string Utf8(const std::wstring& text);
std::string Hex(const uint8_t* bytes, size_t size);
