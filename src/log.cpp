#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

#include "host.h"

namespace {

const CoreHost* g_host = nullptr;

}  // namespace

void LogAttach(const CoreHost* host) {
    g_host = host;
}

void Log(const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (length >= 0 && g_host) {
        g_host->log(text);
    }
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string Hex(const uint8_t* bytes, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 0xF]);
    }
    return out;
}
