#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine.h"
#include "image.h"
#include "objects.h"

// Replacing loaded assets by the versions in a newly mounted container, in place.
//
// The engine's own ReloadPackages was stripped from the game, but its loader still lets a package
// be loaded again once the loaded one has been renamed. So: rename the old packages, load them
// afresh, point everything that used the old assets at the new ones, and let the old ones be collected.
enum class ReloadResult {
    Nothing,  // none of the packages is loaded, so there was nothing to replace
    Done,     // every asset was loaded again and every reference found was moved to the new one
    Partial,  // ran to the end, but some asset or reference was left as it was; the report lists them
    Failed,   // not attempted, or stopped part-way; the report says where
};

class Reloader {
public:
    Reloader(const Image& image, const ObjectSystem& objects) : image_(image), objects_(objects) {}

    // Locates the engine functions involved. Read-only.
    bool Resolve(std::string& report);

    // Calls a harmless script function through ProcessEvent to prove that calling works.
    void CallTest(std::string& report);

    // Lists what a reload of these packages would do. Read-only.
    void Plan(const std::vector<uint64_t>& packageIds, std::string& report);

    // Changes engine state: the whole reload. Renames the loaded packages, loads them again, makes
    // everything that used the old assets use the new ones. The old ones go with the next collection.
    ReloadResult Reload(const std::vector<uint64_t>& packageIds, std::string& report);

    // How many packages that a reload renamed out of the way are still alive. Zero once the old
    // assets have all been collected. Read-only.
    size_t RetiredPackagesAlive() const;

    // Changes engine state: renames the loaded package that holds the asset `assetName` and loads the
    // asset again. Nothing is repointed, so the game keeps using the old copy; this only proves that
    // the two copies can exist side by side.
    void RenameAndLoad(const std::string& assetName, std::string& report);

private:
    const Image& image_;
    const ObjectSystem& objects_;
    EngineCalls calls_;
};
