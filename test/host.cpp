// Stand-in for the game, used to exercise HTReload.asi without launching it.
// It reproduces only what the plugin relies on: the anchor string referenced from the mount
// handler, a heap delegate instance holding { object, method }, the two generated tables that name
// CollectGarbage, and an "UnrealWindow" message pump.
// Built as HTGame.exe under <test root>/HT/Binaries/Win64 so the plugin derives the same paths.

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <iterator>

struct EngineString {
    const wchar_t* data;
    int num;
    int max;
};

__declspec(noinline) void DebugPrint(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    vwprintf(format, args);
    va_end(args);
}

int g_result = -1;
bool g_mounted = false;
DWORD g_mainThread = GetCurrentThreadId();

// --- generated-code look-alikes -------------------------------------------------------------

struct Frame {
    void* reserved[4];
    unsigned char* code;
};

using NativeFn = void (*)(void* context, Frame& stack, void* result);

__declspec(noinline) void ConstructFunction(void** slot, int id) {
    *slot = new int(id);
}

// Like UHT's Z_Construct_UFunction_*: identical shape, hence identical size, for every function.
#define DEFINE_CONSTRUCT(Name, Id)                    \
    void* g_function_##Name;                          \
    __declspec(noinline) void* Construct_##Name() {   \
        if (!g_function_##Name) {                     \
            ConstructFunction(&g_function_##Name, Id); \
        }                                             \
        return g_function_##Name;                     \
    }

DEFINE_CONSTRUCT(BreakA, 1)
DEFINE_CONSTRUCT(BreakB, 2)
DEFINE_CONSTRUCT(CapsuleA, 3)
DEFINE_CONSTRUCT(CollectGarbage, 4)
DEFINE_CONSTRUCT(ComponentA, 5)
DEFINE_CONSTRUCT(ComponentB, 6)
DEFINE_CONSTRUCT(ConvA, 7)

__declspec(noinline) void ExecPrint(void*, Frame& stack, void*) {
    stack.code += !!stack.code;
    wprintf(L"host: exec %p %d\n", static_cast<void*>(&stack), static_cast<int>(GetTickCount64() & 7));
}

__declspec(noinline) void ExecCount(void*, Frame& stack, void*) {
    stack.code += !!stack.code;
    static volatile int calls;
    calls = calls + 1;
}

__declspec(noinline) void ExecBeep(void*, Frame& stack, void* result) {
    stack.code += !!stack.code;
    if (result) {
        *static_cast<int*>(result) = MessageBeep(MB_OK) ? 1 : 0;
        wprintf(L"host: beep %d\n", *static_cast<int*>(result));
    }
}

// Requests a collection the way the engine's thunk does: no arguments read, nothing returned.
__declspec(noinline) void ExecCollectGarbage(void*, Frame& stack, void*) {
    stack.code += !!stack.code;
    if (g_mounted) {
        bool ok = GetCurrentThreadId() == g_mainThread && g_result == 0;
        wprintf(L"host: collect garbage requested after mount -> %s\n", ok ? L"OK" : L"UNEXPECTED");
        g_result = ok ? 0 : 5;
        PostQuitMessage(0);
    }
}

struct LinkInfo {
    void* (*create)();
    const char* name;
};

struct NamePtrPair {
    const char* name;
    NativeFn function;
};

extern const LinkInfo g_linkInfo[] = {
    {Construct_BreakA, "BreakA"},         {Construct_BreakB, "BreakB"},
    {Construct_CapsuleA, "CapsuleA"},     {Construct_CollectGarbage, "CollectGarbage"},
    {Construct_ComponentA, "ComponentA"}, {Construct_ComponentB, "ComponentB"},
    {Construct_ConvA, "ConvA"},
};

extern const NamePtrPair g_natives[] = {
    {"BreakA", ExecPrint},     {"BreakB", ExecCount},     {"CapsuleA", ExecBeep},  {"CollectGarbage", ExecCollectGarbage},
    {"ComponentA", ExecPrint}, {"ComponentB", ExecBeep},  {"ConvA", ExecCount},
};

// --- pak platform file look-alike -----------------------------------------------------------

struct PlatformFile {
    virtual ~PlatformFile() = default;
    virtual const wchar_t* Name() { return L"Base"; }
};

struct PakPlatformFile : PlatformFile {
    const wchar_t* Name() override { return L"PakFile"; }

    __declspec(noinline) void* HandleMountPak(const EngineString& path, int order) {
        DebugPrint(L"Mounting pak file: %s \n", path.data);
        const wchar_t* expected = L"../../../HT/Content/Paks/MOD/hot/hot_1_P.pak";
        bool ok = wcscmp(path.data, expected) == 0 && order == -1 && path.num == static_cast<int>(wcslen(expected)) + 1 &&
                  GetCurrentThreadId() == g_mainThread && !g_mounted;
        wprintf(L"host: mount call order=%d num=%d thread_ok=%d -> %s\n", order, path.num,
                GetCurrentThreadId() == g_mainThread, ok ? L"OK" : L"UNEXPECTED");
        g_result = ok ? 0 : 3;
        g_mounted = true;
        if (!ok) {
            PostQuitMessage(0);
        }
        return this;
    }

    // Second user of the anchor string that is never bound to the delegate.
    __declspec(noinline) void* HandleMountPaksEx(const EngineString& path) {
        DebugPrint(L"Mounting pak file: %s \n", path.data);
        return nullptr;
    }
};

struct RawMethodInstance {
    void* vtable;
    unsigned long long handle;
    PakPlatformFile* userObject;
    void* (PakPlatformFile::*method)(const EngineString&, int);
};

struct Delegate {
    void* instance;
    int size;
};

Delegate g_mountPak;

int wmain(int argc, wchar_t**) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!LoadLibraryW(L"HTReload.asi")) {
        wprintf(L"host: cannot load HTReload.asi (%lu)\n", GetLastError());
        return 4;
    }

    auto* pakFile = new PakPlatformFile;
    if (argc > 5) {
        pakFile->HandleMountPaksEx(EngineString{L"", 1, 1});
    }
    // Registers the generated tables, which keeps them and their functions in the image.
    Frame frame{};
    for (size_t i = 0; i < std::size(g_linkInfo); ++i) {
        g_linkInfo[i].create();
        if (argc > 5) {
            g_natives[i].function(nullptr, frame, nullptr);
        }
    }

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"UnrealWindow";
    RegisterClassW(&windowClass);

    ULONGLONG start = GetTickCount64();
    HWND window = nullptr;
    for (bool running = true; running;) {
        ULONGLONG elapsed = GetTickCount64() - start;
        // The real engine binds the delegate and opens its window some time after plugins load.
        if (!g_mountPak.instance && elapsed > 2500) {
            g_mountPak.instance = new RawMethodInstance{nullptr, 1, pakFile, &PakPlatformFile::HandleMountPak};
            g_mountPak.size = sizeof(RawMethodInstance);
        }
        if (!window && elapsed > 4000) {
            window = CreateWindowExW(0, L"UnrealWindow", L"host", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, nullptr,
                                     nullptr, windowClass.hInstance, nullptr);
        }
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (elapsed > 60000) {
            wprintf(L"host: timed out waiting for a mount call followed by a collection request\n");
            return 2;
        }
        Sleep(16);
    }
    return g_result;
}
