// Stand-in for the game, used to exercise HTReload.asi without launching it.
// It reproduces only what the plugin relies on: the anchor string referenced from the mount
// handler, a heap delegate instance holding { object, method }, the two generated tables that name
// CollectGarbage, and an "UnrealWindow" message pump.
// Built as HTGame.exe under <test root>/HT/Binaries/Win64 so the plugin derives the same paths.

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>

#include "cityhash.h"

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
bool g_collected = false;
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
    if (g_mounted && !g_collected) {
        bool ok = GetCurrentThreadId() == g_mainThread && g_result == 0;
        wprintf(L"host: collect garbage requested after mount -> %s\n", ok ? L"OK" : L"UNEXPECTED");
        g_result = ok ? 0 : 5;
        g_collected = true;
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

// --- object table and name pool look-alikes -------------------------------------------------

// FNameEntryAllocator: the plugin recognises it by the first two entries of block 0.
struct NamePool {
    void* lock;
    unsigned currentBlock;
    unsigned cursor;
    unsigned char* blocks[8192];
} g_namePool;

unsigned AddName(const char* text) {
    if (!g_namePool.blocks[0]) {
        g_namePool.blocks[0] = static_cast<unsigned char*>(calloc(1, 0x20000));
    }
    unsigned short length = static_cast<unsigned short>(strlen(text));
    unsigned short header = static_cast<unsigned short>(length << 6);
    unsigned id = g_namePool.cursor / 2;
    memcpy(g_namePool.blocks[0] + g_namePool.cursor, &header, 2);
    memcpy(g_namePool.blocks[0] + g_namePool.cursor + 2, text, length);
    g_namePool.cursor += (2 + length + 1u) & ~1u;
    return id;
}

// UObjectBase: vtable, flags, index, class, name, outer.
struct Object {
    virtual ~Object() = default;
    unsigned objectFlags = 0;
    int internalIndex = 0;
    Object* klass = nullptr;
    unsigned nameId = 0;
    unsigned nameNumber = 0;
    Object* outer = nullptr;
};

struct Class : Object {
    void* structFields[6] = {};
    int propertiesSize = 0;
    int minAlignment = 8;
};

struct Field : Object {
    Object* next = nullptr;
};

struct Keeper : Object {
    void* slots[4] = {};
};

// FUObjectItem and the FChunkedFixedUObjectArray that GUObjectArray wraps.
struct ObjectItem {
    Object* object;
    int flags;
    int clusterRootIndex;
    int serialNumber;
    int refCount;
};

struct ObjectArray {
    int firstGCIndex;
    int lastNonGCIndex;
    int maxObjectsNotConsideredByGC;
    bool openForDisregardForGC;
    ObjectItem** chunks;
    ObjectItem* preAllocated;
    int maxElements;
    int numElements;
    int maxChunks;
    int numChunks;
} g_objectArray;

template <class T>
T* NewObject(Class* klass, const char* name, Object* outer, int internalFlags = 0) {
    T* object = new T;
    object->klass = klass ? klass : static_cast<Class*>(static_cast<Object*>(object));
    object->nameId = AddName(name);
    object->outer = outer;
    object->internalIndex = g_objectArray.numElements++;
    g_objectArray.chunks[0][object->internalIndex] = {object, internalFlags, 0, 0, 0};
    return object;
}

Class* NewClass(Class* classClass, const char* name, Object* outer, int size) {
    Class* klass = NewObject<Class>(classClass, name, outer);
    klass->propertiesSize = size;
    return klass;
}

const wchar_t kTestPackage[] = L"/game/characters/test/t_body";

// A mounted package with one texture, kept alive directly by one object, through the heap storage
// of another, and by nothing else.
void BuildObjectSystem() {
    AddName("None");
    AddName("ByteProperty");
    g_objectArray.maxElements = 2 * 65536;
    g_objectArray.maxChunks = 2;
    g_objectArray.numChunks = 1;
    g_objectArray.chunks = new ObjectItem*[2]{static_cast<ObjectItem*>(calloc(65536, sizeof(ObjectItem))), nullptr};

    Class* classClass = NewObject<Class>(nullptr, "Class", nullptr);
    classClass->propertiesSize = sizeof(Class);
    Class* packageClass = NewClass(classClass, "Package", nullptr, sizeof(Object));
    Object* corePackage = NewObject<Object>(packageClass, "/Script/CoreUObject", nullptr);
    classClass->outer = corePackage;
    packageClass->outer = corePackage;
    Class* objectClass = NewClass(classClass, "Object", corePackage, sizeof(Object));
    NewClass(classClass, "Field", corePackage, sizeof(Field));
    Class* textureClass = NewClass(classClass, "Texture2D", corePackage, sizeof(Object));
    Class* keeperClass = NewClass(classClass, "Keeper", corePackage, sizeof(Keeper));

    Object* modPackage = NewObject<Object>(packageClass, "/Game/Characters/Test/T_Body", nullptr);
    Object* texture = NewObject<Object>(textureClass, "T_Body", modPackage, 1 << 30);
    Object* otherPackage = NewObject<Object>(packageClass, "/Game/Other", nullptr);
    Keeper* direct = NewObject<Keeper>(keeperClass, "DirectKeeper", otherPackage);
    direct->slots[1] = texture;
    Keeper* indirect = NewObject<Keeper>(keeperClass, "ArrayKeeper", otherPackage);
    void** storage = new void*[8]{};
    storage[3] = texture;
    indirect->slots[2] = storage;
    for (int i = 0; i < 1500; ++i) {
        NewObject<Object>(objectClass, "Filler", otherPackage);
    }
}

// Minimal IoStore table of contents naming one package, as the plugin reads it.
int WriteUtoc(const wchar_t* path) {
    unsigned char toc[144 + 12] = {};
    memcpy(toc, "-==--==--==--==-", 16);
    unsigned headerSize = 144;
    unsigned entryCount = 1;
    memcpy(toc + 20, &headerSize, 4);
    memcpy(toc + 24, &entryCount, 4);
    unsigned long long id = CityHash64(reinterpret_cast<const char*>(kTestPackage), sizeof kTestPackage - sizeof(wchar_t));
    memcpy(toc + 144, &id, 8);
    toc[144 + 11] = 1;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") || !file) {
        return 6;
    }
    fwrite(toc, 1, sizeof toc, file);
    fclose(file);
    return 0;
}

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

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3 && wcscmp(argv[1], L"--write-utoc") == 0) {
        return WriteUtoc(argv[2]);
    }
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
        // Later still, so that the plugin's first look for the object table comes too early and it
        // has to try again when a pak is mounted.
        if (!g_objectArray.chunks && elapsed > 6500) {
            BuildObjectSystem();
        }
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        // The test script ends the run by creating this file once it has seen everything it checks.
        if (GetFileAttributesW(L"host.quit") != INVALID_FILE_ATTRIBUTES) {
            running = false;
        }
        if (elapsed > 120000) {
            wprintf(L"host: timed out waiting to be told to quit\n");
            return 2;
        }
        Sleep(16);
    }
    return g_result;
}
