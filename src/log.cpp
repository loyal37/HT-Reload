#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;

}  // namespace

void LogOpen(const std::wstring& path) {
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, "\xEF\xBB\xBF", 3, &written, nullptr);
    }
}

void Log(const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (length < 0) {
        return;
    }
    if (length >= static_cast<int>(sizeof text)) {
        length = sizeof text - 1;
    }

    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[2100];
    int lineLength = snprintf(line, sizeof line, "[%02u:%02u:%02u.%03u] %.*s\r\n", now.wHour, now.wMinute,
                              now.wSecond, now.wMilliseconds, length, text);

    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, line, static_cast<DWORD>(lineLength), &written, nullptr);
    }
    ReleaseSRWLockExclusive(&g_lock);
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
