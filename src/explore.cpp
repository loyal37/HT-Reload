#include "explore.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "log.h"
#include "probe.h"

namespace {

constexpr size_t kMaxOutput = 4 << 20;
constexpr size_t kSuperStructOffset = 0x40;  // UStruct::SuperStruct

void Append(std::string& out, const char* format, ...) {
    if (out.size() > kMaxOutput) {
        return;
    }
    char text[1024];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (length > 0) {
        out.append(text, std::min<size_t>(static_cast<size_t>(length), sizeof text - 1));
        out += "\r\n";
    }
}

std::string Lower(std::string text) {
    CharLowerBuffA(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

// Explains pointer-sized values: code and data of the game image, and engine objects.
class Annotator {
public:
    Annotator(const Image& image, const ObjectSystem& system) : image_(image), system_(system) {}

    const ArenaVector<ObjectInfo>& Objects() {
        if (objects_.empty()) {
            objects_ = system_.Snapshot();
        }
        return objects_;
    }

    int32_t IndexOf(uintptr_t address) {
        const ArenaVector<ObjectInfo>& objects = Objects();
        auto it = std::lower_bound(objects.begin(), objects.end(), address,
                                   [](const ObjectInfo& object, uintptr_t value) { return object.address < value; });
        return it != objects.end() && it->address == address ? static_cast<int32_t>(it - objects.begin()) : -1;
    }

    int32_t Containing(uintptr_t address) {
        const ArenaVector<ObjectInfo>& objects = Objects();
        auto it = std::upper_bound(objects.begin(), objects.end(), address,
                                   [](uintptr_t value, const ObjectInfo& object) { return value < object.address; });
        if (it == objects.begin()) {
            return -1;
        }
        --it;
        return address < it->address + it->size ? static_cast<int32_t>(it - objects.begin()) : -1;
    }

    std::string Name(int32_t index) {
        return index < 0 ? std::string("?") : system_.Name(Objects()[index].nameId, Objects()[index].nameNumber);
    }

    std::string ClassName(int32_t index) { return index < 0 ? std::string("?") : Name(IndexOf(Objects()[index].klass)); }

    std::string Path(int32_t index) {
        std::string path;
        for (int depth = 0; index >= 0 && depth < 32; ++depth) {
            path = path.empty() ? Name(index) : Name(index) + "." + path;
            index = Objects()[index].outer ? IndexOf(Objects()[index].outer) : -1;
        }
        return path;
    }

    std::string Code(uintptr_t address) {
        uintptr_t end = 0;
        uintptr_t start = FunctionStart(address, &end);
        char text[96];
        if (!start) {
            snprintf(text, sizeof text, "代码 RVA %#zx（无展开信息）", address - image_.base);
        } else if (start == address) {
            snprintf(text, sizeof text, "函数 RVA %#zx 大小 %#zx", start - image_.base, end - start);
        } else {
            snprintf(text, sizeof text, "函数 RVA %#zx +%#zx", start - image_.base, address - start);
        }
        return text;
    }

    std::string Describe(uintptr_t value) {
        if (Image::In(image_.exec, value)) {
            return Code(value);
        }
        char text[64];
        if (image_.Contains(value)) {
            snprintf(text, sizeof text, "主模块数据 RVA %#zx", value - image_.base);
            return text;
        }
        if (int32_t index = IndexOf(value); index >= 0) {
            return "对象 " + ClassName(index) + " " + Path(index);
        }
        if (int32_t index = PlausibleHeapPointer(image_, value) ? Containing(value) : -1; index >= 0) {
            snprintf(text, sizeof text, " +%#zx", value - Objects()[index].address);
            return "对象内部 " + ClassName(index) + " " + Name(index) + text;
        }
        return {};
    }

private:
    const Image& image_;
    const ObjectSystem& system_;
    ArenaVector<ObjectInfo> objects_;
};

void DumpWords(std::string& out, Annotator& annotator, uintptr_t address, size_t bytes) {
    for (size_t offset = 0; offset < bytes; offset += sizeof(uintptr_t)) {
        uintptr_t value = 0;
        if (!SafeRead(address + offset, &value, sizeof value)) {
            Append(out, "+%04zx  （不可读）", offset);
            break;
        }
        Append(out, "+%04zx  %016zx  %s", offset, value, annotator.Describe(value).c_str());
    }
}

// The whole string around a match inside a UTF-16 string table.
uintptr_t WideStringStart(uintptr_t match) {
    uintptr_t start = match;
    for (int i = 0; i < 512; ++i) {
        wchar_t previous = 0;
        if (!SafeRead(start - sizeof previous, &previous, sizeof previous) || previous == 0) {
            break;
        }
        start -= sizeof previous;
    }
    return start;
}

std::string ReadWideString(uintptr_t address) {
    std::wstring text;
    for (int i = 0; i < 200; ++i) {
        wchar_t c = 0;
        if (!SafeRead(address + i * sizeof c, &c, sizeof c) || c == 0) {
            break;
        }
        text.push_back(c == L'\n' ? L' ' : c);
    }
    return Utf8(text);
}

}  // namespace

bool Explore(const std::string& command, const std::string& arguments, const Image& image, const ObjectSystem& system,
             std::string& out) {
    Annotator annotator(image, system);
    std::istringstream words(arguments);
    std::string first;
    std::string second;
    words >> first >> second;
    uintptr_t number = strtoull(first.c_str(), nullptr, 0);
    size_t count = second.empty() ? 0 : strtoull(second.c_str(), nullptr, 0);

    if (command == "objects") {
        std::string wanted = Lower(first);
        size_t limit = count ? count : 60;
        size_t shown = 0;
        const ArenaVector<ObjectInfo>& objects = annotator.Objects();
        for (size_t i = 0; i < objects.size() && shown < limit; ++i) {
            if (Lower(system.Name(objects[i].nameId, objects[i].nameNumber)).find(wanted) == std::string::npos) {
                continue;
            }
            int32_t index = static_cast<int32_t>(i);
            Append(out, "%#zx  %s  %s  标志 %#x 内部 %#x 大小 %#x", objects[i].address, annotator.ClassName(index).c_str(),
                   annotator.Path(index).c_str(), objects[i].objectFlags, objects[i].internalFlags, objects[i].size);
            ++shown;
        }
        Append(out, "共列出 %zu 个（对象总数 %zu）", shown, objects.size());
    } else if (command == "obj") {
        int32_t index = annotator.IndexOf(number);
        if (index < 0) {
            Append(out, "%#zx 不是对象表里的对象", number);
            return true;
        }
        Append(out, "%s  %s", annotator.ClassName(index).c_str(), annotator.Path(index).c_str());
        std::string chain;
        uintptr_t klass = annotator.Objects()[index].klass;
        for (int depth = 0; klass && depth < 32; ++depth) {
            chain += (chain.empty() ? "" : " < ") + annotator.Name(annotator.IndexOf(klass));
            if (!SafeRead(klass + kSuperStructOffset, &klass, sizeof klass)) {
                break;
            }
        }
        Append(out, "类: %s", chain.c_str());
        DumpWords(out, annotator, number, annotator.Objects()[index].size);
    } else if (command == "mem") {
        DumpWords(out, annotator, number, count ? count : 0x100);
    } else if (command == "vtable") {
        uintptr_t table = 0;
        if (!SafeRead(number, &table, sizeof table)) {
            Append(out, "读不到 %#zx", number);
            return true;
        }
        Append(out, "虚表 %s", annotator.Describe(table).c_str());
        for (size_t i = 0; i < (count ? count : 128); ++i) {
            uintptr_t entry = 0;
            if (!SafeRead(table + i * sizeof entry, &entry, sizeof entry) || !Image::In(image.exec, entry)) {
                break;
            }
            Append(out, "[%3zu] +%#05zx  %s", i, i * sizeof entry, annotator.Code(entry).c_str());
        }
    } else if (command == "code") {
        size_t bytes = count ? count : 0x100;
        for (size_t offset = 0; offset < bytes; offset += 32) {
            uint8_t raw[32];
            size_t length = std::min<size_t>(sizeof raw, bytes - offset);
            if (!SafeRead(image.base + number + offset, raw, length)) {
                Append(out, "%#zx 不可读", number + offset);
                break;
            }
            Append(out, "%#010zx  %s", number + offset, Hex(raw, length).c_str());
        }
    } else if (command == "strref") {
        std::wstring wide;
        for (char c : arguments) {
            wide.push_back(static_cast<unsigned char>(c));
        }
        std::vector<uintptr_t> matches = FindBytes(image.readOnly, wide.data(), wide.size() * sizeof(wchar_t));
        Append(out, "含该文字的字符串 %zu 处", matches.size());
        for (size_t i = 0; i < matches.size() && i < 8; ++i) {
            uintptr_t start = WideStringStart(matches[i]);
            Append(out, "字符串 RVA %#zx: %s", start - image.base, ReadWideString(start).c_str());
            size_t faulted = 0;
            for (uintptr_t ref : FindLeaRefs(image.exec, start, &faulted)) {
                Append(out, "    被引用于 %s", annotator.Code(ref - 3).c_str());
            }
        }
    } else if (command == "astrref") {
        std::string pattern = std::string(1, '\0') + arguments + std::string(1, '\0');
        for (uintptr_t match : FindBytes(image.readOnly, pattern.data(), pattern.size())) {
            uintptr_t text = match + 1;
            Append(out, "字符串 RVA %#zx", text - image.base);
            for (const std::vector<Region>* regions : {&image.readOnly, &image.writable}) {
                for (uintptr_t slot : FindBytes(*regions, &text, sizeof text)) {
                    if (slot % sizeof(uintptr_t) == 0) {
                        Append(out, "  指针位于 RVA %#zx", slot - image.base);
                        DumpWords(out, annotator, slot - 2 * sizeof(uintptr_t), 5 * sizeof(uintptr_t));
                    }
                }
            }
        }
    } else if (command == "callers") {
        for (uintptr_t ref : FindBranchRefs(image.exec, image.base + number)) {
            Append(out, "%s", annotator.Code(ref - 1).c_str());
        }
    } else if (command == "names") {
        for (const auto& [id, name] : system.FindNames(first, count ? count : 60)) {
            Append(out, "%#010x  %s", id, name.c_str());
        }
    } else if (command == "refs") {
        ReportReferences(system, number, out);
    } else {
        return false;
    }
    return true;
}
