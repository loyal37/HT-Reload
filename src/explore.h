#pragma once

#include <string>

#include "image.h"
#include "objects.h"

// Read-only questions about the running engine, used to work out layouts and entry points:
//   objects <text> [max]     objects whose name contains text
//   obj <address>            class chain and annotated body of one object
//   mem <address> [bytes]    memory as annotated pointer-sized words
//   vtable <address> [n]     the first n virtual functions of the object at address
//   code <rva> [bytes]       raw bytes of the game image
//   strref <text>            functions that take the address of a UTF-16 string containing text
//   astrref <text>           data that points at the ASCII string text
//   callers <rva>            functions that call or jump straight to the code at rva
//   names <text> [max]       ids of pooled names containing text
//   refs <address>           every engine-owned slot holding that address
// Returns false when `command` is none of these.
bool Explore(const std::string& command, const std::string& arguments, const Image& image, const ObjectSystem& objects,
             std::string& out);
