# Decisions

## 2026-10-10 - Implement "reload the packages and fix up the references" in the plugin

- Decision: the user asked explicitly for a complete in-place reload. Since the engine's own `ReloadPackages` is stripped from the game, the plugin builds the procedure itself: rename the old packages, load the new version, replace references, collect.
- Reason: two probes showed that a character's objects stay alive while the character is off the field, so no kind of switching makes the game let go; and the loader's handling of renamed packages is still there, so the approach works in principle.
- Known cost: the plugin goes from reading to rewriting live objects of the game. Crashes during development become likely and the exposure to the anti-cheat grows. The user was told.
- Alternative: keep only "return to the login screen and enter again".
- Why not: the user wants a complete reload without returning to login.
- Affected: the reload module in the core; `explore.cpp` serves the search for entry points.

## 2026-10-10 - Split into a resident shell and a replaceable core

- Decision: `HTReload.asi` keeps only the log, the game thread hook and the core loader; everything else moves into `HTReload.core.dll`, which the shell replaces when the file changes.
- Reason: the probing had already cost the user four restarts of the game. With many entry points still to work out, a restart per build was not workable.
- Key points: the hook procedure lives in the shell; a reader-writer lock in the shell guarantees that no core code runs on the game thread while the core is unloaded; a copy of the core is loaded, so the original can be overwritten at any time.
- Alternative: stay with a single ASI.
- Affected: `shell/shell.cpp`, `src/host.h`, `src/main.cpp`, `src/game_thread.cpp`, `src/log.cpp`, `build.ps1`, `deploy.ps1`.

## 2026-10-10 - A command file for interactive inspection; probes are no longer automatic

- Decision: the core checks `HTReload.cmd.txt` once a second; the probe and a set of read-only inspection commands are triggered through it and answer into a file. The automatic probe after a mount and its three beeps were removed.
- Reason: having the user package and wait for a sound to trigger a probe was slow and once had them wait for nothing. With the command file the assistant can look repeatedly while the game runs.
- Affected: `Watcher::RunCommands` in `main.cpp`, `explore.cpp`.

## 2026-10-10 - The goal is a complete reload of all assets, not a texture-only refresh

- Decision: the user made clear that the goal is a complete reload of pak assets, not limited to textures. The idea of refreshing textures instantly through re-streaming is shelved.
- Reason: the user's requirement.
- Effect: the work is about making the game let go of old assets and load them again, not about shortcuts for one kind of asset.

## 2026-10-10 - Probe read-only first, to learn who holds the old assets

- Decision: v0.3.0 reads the engine's object table and name pool and reports, after each mount, whether the old objects are alive and who references them. Facts first, then the design of the reload.
- Reason: switching within the party, leaving the party, changing the skin and teleporting all failed to refresh anything although active collection worked; only returning to login did. From the outside it could not be told who held the old assets.
- Alternatives: have the user try more in-game actions; use an existing tool such as UE4SS.
- Why not: the first was exhausted; the second may not work on a packed, custom engine with anti-cheat and is exactly what anti-cheat looks for.
- Affected: `objects.cpp`, `probe.cpp`, `cityhash.cpp`, `arena.cpp`.

## 2026-10-10 - The probe reads only memory that can be attributed to the engine

- Decision: the reference scan covers the image's writable data plus the memory allocations that hold at least one engine object. Other memory (heaps of other DLLs, memory of drivers and of the anti-cheat, and so on) is not read.
- Reason: scanning the whole process is a large action and may touch memory that should be left alone; and the engine's small heap blocks mostly share allocations with objects.
- Cost: a container whose storage sits in a large allocation of its own, with no object in it, is missed. Should a probe show old objects alive without any referrer, widening is to be reconsidered.
- Affected: `EngineMemory` in `probe.cpp`.

## 2026-10-10 - No bridge for now; packaging changes go into the packager

- Decision: the user said NTE Bridge is not used for now and packaging stays with `NTE-Mod-Packager`. Automatic numbering and cleanup of old versions are packager changes.
- Reason: the user's actual workflow is the packager publishing straight into the game's `Paks\MOD\<mod name>`.
- Alternative: change the bridge's publish step.
- Why not: the user does not use the bridge at present.
- Affected: `BuildService.PublishWithRollbackAsync` and the overwrite prompt of the main window in `D:\Neverness to Everness Mod Loader\cook\packager`. No code of this project.

## 2026-10-10 - Leave the packager alone during validation; rename by hand

- Decision: the first validation relies on the ASI alone. The user types `name_1_P`, `name_2_P` into the packager's name field.
- Reason: the packager publishes a new name without trouble as it is; hot reload had to be shown to work before investing in the packager.
- Alternative: change the packager first.
- Why not: the big unknown was whether the game releases old assets, which has nothing to do with the packager.
- Affected: nothing.

## 2026-10-10 - Mount under a new name; no unmount, no overwrite

- Decision: every hot publish mounts a new container with a higher version number; old containers stay mounted until the game exits.
- Reason: the game opens containers without `FILE_SHARE_DELETE`, so a file of the same name cannot be overwritten; unmounting a container that still has textures streaming from it may crash. The engine raises the priority of `_N_P` names by itself, so a new version always outranks an old one.
- Alternative: unmount the old container, overwrite the files, mount again.
- Why not: that needs every asset of the container released first, which is more risk and more work.
- Affected: `Watcher` in `main.cpp`, and later packager changes.
- Superseded in part on the same day: see the two entries on unmounting at the end.

## 2026-10-10 - Call the engine's own MountPak handler, located by a string and the delegate instance

- Decision: no hard-coded addresses or signatures. The string `Mounting pak file: %s \n`, which survives in shipping builds, gives the candidate functions, and the delegate instance of `FCoreDelegates::MountPak` confirms the function and yields the `FPakPlatformFile` object.
- Reason: the game's code is encrypted on disk, so signatures cannot be taken offline; the game updates often (the SigBypasser log recorded 6 different hit addresses between 2026-08-16 and 10-09); this route yields function and object together without assuming any struct offsets.
- Alternatives: a signature scan for `FPakPlatformFile::Mount`; an inline hook on `Mount` at startup to capture `this`; walking the `FPlatformFileManager` chain.
- Why not: signatures break with updates; an inline hook changes game code; the last depends on vtable indices.
- Affected: `resolver.cpp`.

## 2026-10-10 - No evasion of the anti-cheat

- Decision: the plugin reads process memory and calls functions the engine already has; it does not modify game code and does nothing to hide from or get around the anti-cheat.
- Reason: this is a development tool for personal use; the less it does the more stable it is, and the anti-cheat is not something to touch.
- Note: the original wording was "read only, change nothing". Since the in-place reload the plugin does write to engine objects (pointers in object bodies); it still changes no code.
- Affected: everything.

## 2026-10-10 - A thread message hook to get onto the game thread

- Decision: a `WH_GETMESSAGE` thread hook on the thread that owns the `UnrealWindow`, with `PostThreadMessage` posting a custom message; the work runs when the engine fetches its messages.
- Reason: UE fetches messages once per frame on the game thread, outside the world tick; no further engine function has to be located.
- Alternatives: replace the window procedure; hook the engine's tick.
- Why not: replacing the window procedure interferes with other tools that subclass the window (ReShade, for example); hooking the tick needs another address and changes code.
- Affected: `game_thread.cpp`.

## 2026-10-10 - Poll the directory, no notification file

- Decision: scan `Content\Paks` once a second and take a file as complete when it can be opened for exclusive reading and its size is the same twice in a row.
- Reason: the packager writes temporary files and renames them, so a file is complete the moment it appears; the packager needs no change.
- Alternatives: a request file written by the packager; `ReadDirectoryChangesW`.
- Why not: the first changes the packager; the second is more code, and with a few dozen files in the directory the cost of polling is negligible.
- Affected: `Watcher` in `main.cpp`.

## 2026-10-10 - INDEX_NONE as the order argument

- Decision: the handler is called with `PakOrder` set to `-1`.
- Reason: the engine then computes the order with the same `GetPakOrderFromPakFilePath` as its startup scan, so the result matches a mod mounted at startup exactly.
- Alternative: pass 3 explicitly.
- Why not: that is an extra assumption about the game's directory structure.
- Affected: `CallHandleMountPak` in `main.cpp`.

## 2026-10-10 - Request collections through execCollectGarbage after a mount

- Decision: from v0.2.0, `UKismetSystemLibrary::execCollectGarbage` is called every 5 seconds for 120 seconds after a successful mount. This replaces the earlier position of the same day that v0.1 forces no collection.
- Reason: in the game the mount succeeded but switching characters within the party refreshed nothing. Without active collection, "no change" could mean either not unloaded or not collected yet.
- Why this entry: it only asks the engine to collect on its next frame and needs no GC lock; it takes no parameters, so a zeroed fake script frame is enough to call it; and it can be located reliably through the name tables UHT generates.
- Alternatives: call `CollectGarbage` directly; find `UEngine::ForceGarbageCollection` and `GEngine`; bind a hotkey.
- Why not: the first has to be called at the right moment and lock state; the second function is too small and has no usable anchor; a hotkey needs keyboard polling or another hook.
- Affected: `FindCollectGarbageThunk` in `resolver.cpp`, `Watcher::RequestCollection` in `main.cpp`.

## 2026-10-10 - Deploy into the Win64 root

- Decision: `HTReload.asi` and `HTReload.log` live in the root of the game's `Win64` folder, next to `dsound.dll` and `Anticensor.asi`.
- Reason: that is where the user looks at these files.
- Alternative: `Win64\plugins`, which the loader loads as well.
- Affected: `deploy.ps1`.

## 2026-10-10 - Scripts stay ASCII, sources are compiled with /utf-8

- Decision: `.ps1` files use ASCII only; C++ sources are UTF-8 without BOM and compiled with `/utf-8`.
- Reason: Windows PowerShell 5.1 reads BOM-less scripts in the system code page, which damages other characters and can even break the syntax.
- Affected: `build.ps1`, `deploy.ps1`, `test\run.ps1`.

## 2026-10-10 - Project memory in `.ai-memory`

- Decision: following `D:\ueproject\HT\.ai-memory`, this project keeps the same seven files in its root, with the `project-memory-keeper` skill.
- Reason: the user asked for it.
- Affected: the project's workflow.

## 2026-10-10 - In-place reload: rename, load again, replace references, in one game-thread task

- Decision: old packages are renamed out of the way with `UObject::Rename` and loaded again with `LoadAsset_Blocking`, then the slots that point at old assets are given the new ones, and finally collections are requested. The first three steps run in one game-thread task.
- Reason: the user asked for "reload the packages and fix up all references" inside the plugin. The engine's `ReloadPackages` is stripped, but the loader's handling of renamed packages is not. One task, because the new objects must not meet a collection before something references them.
- Alternatives: return to the login screen (works, but interrupts play); re-stream textures only (the user ruled out a texture-only solution).
- Affected: `src/reload.cpp`, `src/engine.cpp`.

## 2026-10-10 - Dynamic materials keep their object and get a new parent

- Decision: a dynamic material is not recreated; its `Parent` is changed and `CopyParameterOverrides(new parent)` is called.
- Reason: the game keeps dynamic materials in containers of its own and would not recognise new objects. `CopyParameterOverrides` is the only way found to trigger `InitResources()` from a script function.
- Cost: the parameters the game had set on the dynamic material are cleared.
- Affected: `src/reload.cpp`.

## 2026-10-10 - Where a value also reads as a name id, only a declared object property counts as a slot

- Decision: of an object's header only the class pointer is a slot. Where an old object's address is also the id of an entry in the name pool, the slot is rewritten only if the owner's class (or a parent) declares an `ObjectProperty` / `ClassProperty` at that offset; otherwise it is marked doubtful and left.
- Reason: a name field was in fact mistaken for a pointer during testing. Writing an address into a name field renames an unrelated object, with effects that are hard to trace.
- Trade-off: a declared property is not required for every slot, because raw pointers in native fields (the component's +0xdf8, for example) have to be rewritten too, or they dangle once the old object is collected. The rule is only tightened where there is ambiguity.
- Affected: `Classify` in `src/reload.cpp`, `IsNameId` in `src/objects.cpp`, `FindSuperseded` in `src/probe.cpp`.

## 2026-10-10 - Reload automatically after a mount, on by default, no switch

- Decision: when a new container has been mounted, its packages are reloaded right away. If the reload does not complete, the log says so and the older way of returning to the login screen remains.
- Reason: the user's goal is to see the result right after packaging. The manual reload had succeeded twice in the game.
- This was the assistant's judgement; the user had not asked for it to be automatic in so many words. There is no switch; one can be added when the user wants it.
- Affected: `Watcher::ReloadMounted` in `src/main.cpp`.

## 2026-10-10 - Unmount old containers after a reload, without deleting files

- Decision: once a reload is done and the old assets are collected, the engine's unmount handler is called for old containers that the new version fully supersedes. The plugin deletes no files.
- Reason: the user asked whether the old three files can be deleted while the game runs (measured: they cannot) and then asked for old containers to be unmounted automatically after a reload. Unmounting is what was asked; deleting cannot be undone and was not asked for.
- The rule errs on the careful side:
  - Only a container whose every package is also in another mounted container with a strictly higher number is unmounted. One missing package is enough to keep it, since that package would otherwise fail to load later or fall back to the game's own version.
  - Equal numbers are not unmounted. With equal order the engine prefers the one mounted later, but which of two containers mounted together at startup came first is not known.
  - All packages carrying `_HTOLD_` have to be collected first. Old assets that are still alive could ask for data blocks the new version does not have.
  - "Mounted" looks both at the plugin's own record and at whether the file is held open, so that a new pak the packager is still writing is not taken for mounted, which would unmount the current version too early.
- Alternatives: clean up only after the game exits (does not give deletion at run time); delete right after the unmount (not asked for).
- Affected: `src/resolver.cpp`, `src/main.cpp`, `test/host.cpp`, `test/run.ps1`.

## 2026-10-10 - Delete the old three files right after the unmount (replaces "no deleting" above)

- Decision: when the engine has unmounted an old version and its file is really let go, the plugin deletes its `.ucas`, `.utoc` and `.pak` with `DeleteFileW`. Permanently, not to the recycle bin.
- Reason: the user confirmed in the game that the files could be deleted after the unmount and then asked for them to be deleted directly, without waiting for a manual step.
- Because deleting cannot be undone, a guard was added with it: the container that supersedes must not have been mounted before the one it supersedes. Otherwise a new pak published with too low a number would be unmounted and deleted as an old version. Such a pak now only gets a note in the log.
- Not done: the recycle bin (the user said to delete directly, and at about 360 MB a version the recycle bin would not free the space either); a switch.
- Affected: `src/main.cpp`, `test/host.cpp`, `test/run.ps1`, README.

## 2026-10-11 - A material instance that refers to a reloaded asset is reloaded, not patched

- Decision: when a material instance from a package refers to a superseded asset (its parent, or a texture in its parameter arrays), its package joins the set that is renamed and loaded again. Only where that is not possible, or beyond 64 such packages, is the pointer rewritten in place.
- Reason: the render thread works from a copy of the instance's parameters that is made when the instance is initialised. Rewriting the pointer on the game thread would leave that copy on the old texture, and no script function re-initialises a constant material instance. Loading it again does, with the engine's own code, and uses only operations already proven in the game.
- Alternatives: call `PostLoad` on the instance a second time through its vtable; find `InitResources` natively.
- Why not: the first relies on a function that is not meant to run twice; the second has no anchor to locate it by.
- Limit: only instances, not base materials, and only what the array walk finds.
- Affected: `Gather` in `src/reload.cpp`, `FindSuperseded` in `src/probe.cpp`.

## 2026-10-11 - Override materials follow the change between two meshes, not the state of one

- Decision: after a mesh reset, an override is moved only in slots where the new mesh's material differs from the old mesh's, and only if the override is that old material or a dynamic material derived from it.
- Reason: the game, or a mod's blueprint, may put an override into a slot on purpose (toggles, effects). Forcing every override to match the mesh would undo that. Comparing the two meshes singles out what the new version changed.
- Cost: an override that is already out of step is not corrected by a later reload.
- Affected: `followSlots` in `Reloader::Reload`.

## 2026-10-11 - Members of property subclasses are found by content

- Decision: the offsets of `FArrayProperty::Inner` and `FStructProperty::Struct` are not written down; they are found at run time from two properties every UE game has (`MeshComponent.OverrideMaterials`, `SkeletalMesh.Materials`).
- Reason: same as for the entry points: the game's engine is modified and updates often.
- Affected: `src/reflect.cpp`.
