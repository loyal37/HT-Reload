# Architecture

## Directory layout

- `shell/shell.cpp`: the shell, built as `HTReload.asi`.
- `src/`: the core, built as `HTReload.core.dll`. `src/host.h` is the contract between the two.
- `test/host.cpp`: a stand-in host, built as `HTGame.exe`, so the plugin can be tested without the real game.
- `test/run.ps1`: the end-to-end test.
- `build.ps1`: builds shell and core; `-Test` also builds the stand-in host.
- `deploy.ps1`: copies to the game's Win64 folder. The core can be deployed at any time; a changed shell is refused while the game runs.
- `tools/ask.ps1`: sends commands to the running game and prints the answers. `tools/disasm.ps1`: turns the bytes returned by `code` into assembler data, assembles them with `ml64`, disassembles with `dumpbin /disasm` and converts the addresses back to RVAs in the game.
- `build/`: build output, not tracked.
- `.ai-memory/`: persistent project memory.
- `.codex/skills/project-memory-keeper/`: the skill that maintains this memory.

## Shell and core (v0.4.0)

- The shell is loaded by the ASI loader, stays for the life of the process and only acts inside `HTGame.exe`. It does three things:
  - The log file (at startup the previous one is renamed to `HTReload.prev.log`).
  - Dispatch to the game thread: a `WH_GETMESSAGE` hook on the thread that owns the `UnrealWindow`; the hook procedure lives in the shell. The task queue only holds "function pointer + context".
  - Loading the core: `HTReload.core.dll` is copied to `HTReload.core.run<N>.dll` and the copy is loaded, so the original can be overwritten at any time. Once a second the original's modification time and size are checked; when they are the same twice in a row and differ from what is loaded, the core is replaced.
- Replacing the core: call the core's `CoreStop` (it raises its stop event and waits for its worker thread, at most 60 seconds) → the shell takes the exclusive lock and clears the task queue, which guarantees that no core code runs on the game thread from then on → `FreeLibrary` and delete the copy → load the new one. A core that does not stop in time is left loaded and only logged.
- The shell holds the shared lock while it runs tasks on the game thread; a task that enters the message loop again is not re-entered.
- The core exports `CoreStart(const CoreHost*)` and `CoreStop()`. `CoreHost` carries the contract version, the load count, the plugin directory, the log function, whether the game thread is ready, and the task poster.
- Every time the core starts it locates the mount, unmount and collection entry points again and takes every pak on disk as mounted. Only the first load in a process waits 3 seconds first.
- Every wait in the core can be interrupted by the stop event, including the wait for a mount result.

## Core modules

- `main.cpp`: `CoreStart` starts the worker thread, which locates the entry points, snapshots the directory, waits for the game thread and then polls once a second. `Watcher` finds new paks and has them mounted, reloads, unmounts and deletes old versions, requests collections and runs the command file.
- `resolver.{h,cpp}`: locates the mount handler in the running game (`MountApi { handleMountPak, pakPlatformFile, handleUnmountPak }`), the unmount handler and the collection thunk.
- `image.{h,cpp}`: reads the main module's memory. Committed memory is grouped by page protection into executable, read-only and writable; offers byte search, `lea [rip+disp32]` reference scans and function start lookup (`RtlLookupFunctionEntry`, following chained unwind info). Every read is guarded by SEH.
- `objects.{h,cpp}`: `ObjectSystem` (name pool and object table) and `ObjectView`, one snapshot of the object table with lookup by address, names, paths and class inheritance. New code uses `ObjectView`.
- `probe.{h,cpp}`: package ids of a container, the set of superseded objects, and the read-only reference report.
- `engine.{h,cpp}`: locates and calls the engine functions a reload needs.
  - `UObject::Rename`: its log text sits in a static record, and the code references the record, not the text. Find the text, then the pointer to it (the record), then the function that references the record. Then sample about 400 objects, see which vtable slot it occupies and take the majority.
  - `UObject::ProcessEvent`: the vtable entry whose first 0x80 bytes contain `test dword ptr [rdx+offset], 400h` (the check for a native function), again by majority; that offset is the offset of `UFunction::FunctionFlags`.
  - Calls go through the object's own vtable, so overrides in subclasses are reached correctly. Calls are wrapped in SEH.
- `reload.{h,cpp}`: `Reloader`. Implements the commands `engine`, `calltest`, `plan`, `renameload` and `reload`, the classification of slots, and the reload itself. See "In-place reload" below.
- `explore.{h,cpp}`: read-only inspection commands. Can explain a pointer as "function + offset", "image data", "an object" or "inside an object".
- `game_thread.{h,cpp}`: the core's side of the task queue, posted to the game thread through the shell.
- `log.{h,cpp}`: formats and hands lines to the shell's log.
- `arena.{h,cpp}`: the probe's own tables are allocated with a separate `VirtualAlloc`, so a scan can recognise and skip them.

## Locating the mount handler

1. Find the UTF-16 string `Mounting pak file: %s \n` in read-only data. `HandleMountPakDelegate` and the MountPaksEx handler pass it to `LowLevelOutputDebugStringf`; it is not a `UE_LOG`, so it survives in shipping builds.
2. Find the `lea` instructions that reference it in the code and take the start of their functions: the candidates (two in the stock engine).
3. Scan every pointer in the image's writable data and look, in the first 12 machine words of the heap block it points at, for an adjacent pair "object pointer + candidate function address". That is the layout `{ UserObject, MethodPtr }` of the delegate instance of `FCoreDelegates::MountPak`. The object has to be a heap pointer whose vtable lies in the image.
4. The function that matches is `HandleMountPakDelegate`, and the word before it is the `FPakPlatformFile*`. Only that function is bound to the delegate directly; the other candidate never matches.

The code is only unpacked while the game starts, and the delegate is only bound once the engine has initialised its pak system, so locating is retried: wait 3 seconds, then 15 tries one second apart, then every 5 seconds, giving up after 300. While fewer than 2 candidates are known, every try rescans.

## The mount call

- Signature: `IPakFile* HandleMountPakDelegate(FPakPlatformFile* this, const FString& PakFilePath, int32 PakOrder)`.
- `FString` is built as `{ wchar_t* Data; int32 Num; int32 Max }`, with `Num` counting the terminating null.
- The path is spelled the way the engine spells it: `../../../<Project>/Content/Paks/<relative path>`, forward slashes. `<Project>` is the name of the directory two levels above the exe (`HT` in the game).
- `PakOrder` is `-1` (`INDEX_NONE`), which makes the engine work out the order by the same rule as its startup scan.
- The call is wrapped in SEH; an exception is logged and stops all further mounts.

## Collection requests

- Target: `UKismetSystemLibrary::execCollectGarbage`. It runs `GEngine->ForceGarbageCollection(true)`, which only raises a flag; the engine does the full collection itself on its next tick.
- Locating: find the ASCII name `CollectGarbage` in read-only data (null before and after, exactly one occurrence), then the 8-byte pointers to it. Two tables generated by UHT hold the name:
  - `FClassFunctionLinkInfo { construct function, name }`: the functions of neighbouring entries all have the same size.
  - `FNameNativePtrPair { name, exec function }`: the functions of neighbouring entries differ in size, and the function right after the name is the one wanted.
  - Take the function sizes of the 3 entries before and after the name; a table where they are all equal and non-zero is the construct table and is skipped.
- Calling: `thunk(nullptr, frame, nullptr)`, with `frame` a zeroed 0x200-byte buffer standing in for an `FFrame`. Its `Code` pointer is null, so `P_FINISH` does nothing. A static function does not use `Context`.
- Rhythm: every 5 seconds for a window after a mount, on the game thread: 20 seconds after a completed reload, 120 seconds otherwise. The call is wrapped in SEH; an exception disables the feature.
- RVAs checked offline for the 2026-10-01 build: name `0xbf660a0`, construct table entry `0xbf50a08` (all function sizes 0x2f), native table entry `0xbfb57e0`, exec function `0x4406bd0` (a leaf function without unwind info).
- The game has a function of its own called `ForceGarbageCollection`, but it shares its address with several neighbours: an empty stub in the shipping build, unusable.

## In-place reload (core 0.6.x, verified in the game on 2026-10-10)

- Entry: `Reloader::Reload(package ids, report)`, which returns `Nothing` (none of the packages is loaded), `Done`, `Partial` (since 0.8.0) or `Failed`. `Watcher::ReloadMounted` calls it after a successful mount; the command `reload [filter]` triggers it by hand. `plan [filter]` only lists what would be done and changes nothing.
- Preparation (worker thread, read-only): `FindSuperseded` finds the loaded packages the new container covers, every object inside them, and the slots in the bodies of other objects that point at those objects; `PrepareWork` works out everything that is to be done.
  - The assets to load are the public objects directly inside the packages, class default objects excepted.
  - Slot classification (`Classify`):
    - leave: the slot's owner is an instance of an old class (the class pointer of an anim blueprint instance, for example); it goes away with the old assets.
    - doubtful: the old object's address also reads as a name id, and the owner's class declares no object reference at that offset. Never written.
    - reparent: the owner is a `MaterialInstanceDynamic` and the slot is its `Parent` (the offset is looked up through reflection, +0x128 in the game).
    - reset mesh: the owner is a `SkinnedMeshComponent` in a level (not a template) and the target is a `SkeletalMesh`.
    - rewrite: everything else; the new pointer is stored directly.
- Execution (game thread, all in one task with a 120 second timeout, so that the newly loaded objects cannot meet a collection before something references them):
  1. Rename every old package: `UObject::Rename` to `<name>_HTOLD_<tick count in hex>`, flags 0x35.
  2. `KismetSystemLibrary.LoadAsset_Blocking` for each asset; the package and asset names in the soft reference are the name ids of the old objects. The loader notices that the loaded package was renamed, drops its record and reads from disk again, from the container with the highest order.
  3. Rewrite slots: before the new pointer is stored, the slot is checked to still hold the old one (`ReplacePointer`, with SEH).
  4. Reparent: store the new `Parent`, then call `MaterialInstanceDynamic.CopyParameterOverrides(new material)`, which runs `InitResources()` and so pushes the new parent to the render thread. As a side effect the parameters the game had set on that dynamic material are cleared.
  5. Reset mesh: one `SkeletalMeshComponent.SetSkeletalMeshAsset(new mesh)` per component; afterwards any slot of that component that still holds the old mesh is rewritten.
- Then a 20 second collection window, in which the old objects all go away.
- If the engine raises an exception at any step, the reload stops there and the report says where, with the exception code.
- Reports: an automatic reload writes `Win64\HTReload.reload.txt` and its summary lines go to the log (prefix `Reload:`); the report of a manual command is in `HTReload.out.txt`.
- Sounds: two rising tones for a successful mount; one more, higher, when the reload is done; one low tone when the reload did not complete or the mount failed.

## Array references, dependent materials, slot materials (core 0.8.0)

- `reflect.{h,cpp}`: `Reflection` reads `UStruct` / `FField` / `FProperty`. The members that subclasses of `FProperty` add are found by content rather than by fixed offsets: `FArrayProperty::Inner` is the pointer, between 0x48 and 0xC0 of `MeshComponent.OverrideMaterials`, to a property of the same name that holds objects; `FStructProperty::Struct` is the pointer in the inner property of `SkeletalMesh.Materials` that equals the script struct `SkeletalMaterial`.
- `ArraysOf(class)` lists, cached per class, the top-level array properties of a class and its parents whose elements hold object references: arrays of object pointers, and arrays of structs with object pointers in them (nested structs up to three levels). `FindArrayReferences(targets)` walks those arrays for every object outside the target set, about 390,000 objects in the game, in well under a second.
- `Gather` in `reload.cpp` widens the superseded set: a material instance from a `/Game/` package (not a dynamic one) that refers to a superseded asset, through a slot in its body or through an array, has its package added to the set, and the search runs again, for up to 8 rounds and 64 such packages. The reason: a material instance's render state is built when it is loaded and cannot be brought up to date from script, so it is reloaded as a whole. Such packages are loaded from the game's own containers; their imports resolve to the new assets because every rename happens before any load.
- Array references that remain are classified (`ArrayKind`): an override material of a live mesh component is set with `PrimitiveComponent.SetMaterial`; a texture parameter of a dynamic material with `MaterialInstanceDynamic.SetTextureParameterValueByInfo` (the 0x10-byte `ParameterInfo` is copied from the element); anything else has the pointer in the element rewritten. On the game thread the array is read again and every element that still holds the old object is dealt with, so a reallocation between planning and execution does no harm.
- Following slot materials: after `SetSkeletalMeshAsset` on a live component, the materials of the old and the new mesh are compared slot by slot (`SkeletalMesh.Materials`, stride `sizeof(FSkeletalMaterial)`, field `MaterialInterface`). Where the new mesh uses a different material, and not merely the reloaded copy of the old one, the component's override for that slot is moved: a plain override equal to the old material is set to the new one; a dynamic material whose `Parent` is the old material is reparented. Any other override is left and noted. The rule works on the change between two meshes, so an override that was already out of step before a reload is not corrected.
- `ReloadResult::Partial`: the run reached its end but an asset produced no new object, or a slot or array element that was not marked "leave" was not settled. The watcher logs it and plays a high tone followed by a lower one.
- Measured in the game (2026-10-11): `SkeletalMesh.Materials` at +0x1b0 with 0x30-byte elements; texture parameter elements are 0x18 bytes with the texture at +0x10; a component's `OverrideMaterials` at +0x548.

## Unmounting and deleting old containers (unmount in core 0.7.0, delete in 0.7.1)

- Purpose: the engine keeps the `.ucas` and `.utoc` of a mounted container locked, so an old version cannot be deleted while the game runs. After an unmount the engine closes the files, and the plugin deletes the three files (`DeleteFileW`, not the recycle bin).
- Deleting (`Watcher::DeleteContainer`): only when the engine returned "unmounted" and the `.ucas` is really no longer held. Order `.ucas`, `.utoc`, `.pak`: the data file first, and if it refuses, stop, so the container stays complete. A file that cannot be deleted is logged with its error code.
- Locating (`FindUnmountHandler` in `resolver.cpp`): the log text `Unmounting pak file: %s \n` (exactly one in the game's file, right next to the mount text) → the functions that reference it → in the image's writable data and the heap blocks it points at, the adjacent pair "the known `FPakPlatformFile` object + that function", which is the delegate instance of `FCoreDelegates::OnUnmountPak`. There has to be exactly one. Looked for once at startup, and again when needed if that failed.
- Calling: `bool HandleUnmountPakDelegate(FPakPlatformFile* this, const FString& PakFilePath)`, the path spelled as for mounting, on the game thread, wrapped in SEH; an exception disables further unmounts.
- Which containers (`Watcher::ShadowedContainers`): a mod container A for which there is another mounted mod container B with a strictly higher patch rank, mounted no earlier than A, that holds every package id of A.
  - Mount sequence: 0 for everything on disk at startup, then one more for each successful mount in this run (the values of `mounted_`). This keeps a new pak that was given too low a number from being deleted as an old version. Replacing the core resets all sequences to zero.
  - When a new pak is mounted while a container with a higher number holds some of the same packages, the log gets a note that it will not take effect.
  - Mounted means: the plugin has it down as mounted (on disk at startup, or mounted successfully in this run) and its `.ucas` is currently held open so that it cannot be deleted (`HeldOpen`, which only asks for delete access and deletes nothing). Both are needed, because a file the packager is still writing is held open as well.
  - `PatchRank` copies the rule of the engine's `FPakPlatformFile::Mount`: 0 without `_P`, N+1 for `<name>_<N>_P`, 1 for any other `_P`.
- When (`Watcher::UnmountWhenCollected`): after a reload that ended as done or as "nothing to reload", look after 6 seconds, then every 5 seconds, for at most 30 seconds. The condition is that no package with `_HTOLD_` in its name is alive (`Reloader::RetiredPackagesAlive`). If some still are at the end, give up and leave the old containers mounted. Nothing is unmounted after a failed reload.
- After the unmount, `HeldOpen` checks that the file was really let go, and only then is it deleted. The outcome goes to the log (prefix `Unmount:`): unmounted and deleted; unmounted but a file could not be deleted; unmounted by the engine but still held open, nothing deleted; the engine did not unmount it.
- Commands: `shadowed` only lists, `unmount` runs the same logic by hand (and deletes files as well).

## Object system and reference probe (v0.3.0, read-only diagnosis)

- `objects.{h,cpp}`: `ObjectSystem` finds two things in the image's writable data by their shape, without relying on addresses.
  - Name pool: the first block of `FNameEntryAllocator::Blocks` starts with the entries `None` and `ByteProperty`. Entry header, 16 bits: the lowest bit says wide characters, the top 10 bits are the length. In a name id the high 16 bits are the block number and the low 16 bits, times 2, the offset in the block.
  - Object table: `FChunkedFixedUObjectArray { Objects, PreAllocated, MaxElements, NumElements, MaxChunks, NumChunks }`. Confirmed by counts that agree with each other and by the first 64 items pointing at objects whose `InternalIndex` equals their position. Item sizes tried in turn: 0x18 (unpacked), 0x10 (`UE_PACK_FUOBJECT_ITEM` packed pointers), 0x20, 0x28.
  - Object header as in UE 5.6: vtable 0x00, `ObjectFlags` 0x08, `InternalIndex` 0x0C, class 0x10, name 0x18 (id and number, 4 bytes each), outer 0x20.
  - An object's size is its class's `PropertiesSize`. The offset of that field is calibrated with the classes `Object` (0x28) and `Field` (0x30) plus the alignment value 8 that follows; in the game it is 0x58, as in the stock engine. The alignment `MinAlignment` is only 16 bits wide (another 16-bit value, 1, follows), so only the low 16 bits may be compared; v0.3.2 compared 32 bits and failed. When calibration fails, sizes are estimated from the gaps between neighbouring objects.
  - The layout of `UStruct` in the game matches the stock engine: `SuperStruct` 0x40, `Children` 0x48, `ChildProperties` 0x50, `PropertiesSize` 0x58, `MinAlignment` 0x5C.
  - The object table only exists once the engine has initialised its object system, some ten seconds after the pak system, so it is looked for once the game window exists and again at a mount or a command if that failed. The name pool exists very early.
  - Name ids reach into the range of low heap addresses, and objects do get allocated that low. `ObjectSystem::IsNameId` tells whether a name entry starts at a given id, by walking the block from its beginning.
  - Measured in the game (2026-10-01 build): name pool Blocks RVA `0xfa36e10`; object table RVA `0xfb1af80` (the `A @ RVA 0xFB1AF70` in Anticensor's log is the start of `GUObjectArray`, 0x10 bytes before it); 0x18-byte unpacked items; objects created early in startup sit at high addresses such as `0x7ff4…`.
- `probe.{h,cpp}`:
  - Reads the package ids from a new container's `.utoc`: a 144-byte header (magic `-==--==--==--==-`, header size at offset 20, entry count at 24), then one 12-byte chunk id per entry, of which the first 8 bytes are the id and the last byte the type; type 1 is package data.
  - Package id = `CityHash64` of the lower-case UTF-16 package name; the implementation is in `cityhash.cpp` and was verified against a real `blackbird_2_P.utoc`, where all 6 package names tried were found.
  - An object without an outer is a package. Loaded packages whose name hash is among the new container's ids are the ones it supersedes; objects whose outermost is such a package are the "old objects".
  - Scan range: the image's writable data plus every memory allocation that holds at least one engine object. Memory that cannot be attributed to the engine this way is never read.
  - First pass, slots equal to the address of an old object: those in the object table are ignored, those inside old objects count as internal, those inside other objects are reported as direct referrers, and the rest are "pointers outside objects" (mostly the heap storage of `TArray` / `TMap`).
  - Second pass, owners for the pointers outside objects: the addresses up to 0x800 bytes before such a slot, without crossing another object, are possible heap block starts; another scan looks for objects or image globals that point at them.
  - In an object's header only the class is a reference to an object elsewhere; `FindSuperseded` does not report the other header words as slots.
- Package paths: this mod's package names are `/Game/Characters/Player/042_Blackbird_1/<name>`, written inside the assets. The packaging source directory `xg\HT\Content\Characters\042_Blackbird_1` lacks the `Player` level, and the directory index of the `.utoc` follows the disk, so package names cannot be derived from the directory index.

## Data flow

1. The game starts and `dsound.dll` loads `HTReload.asi`.
2. Once the worker thread has located the handler, every pak under `Content\Paks` is recorded as handled. The delegate is bound after the engine's startup scan, so everything on disk at that moment has been mounted by the engine.
3. When the game window exists, the thread hook is installed and the log says `Ready`.
4. One scan a second. A new pak qualifies when its utoc and ucas are either both there or both absent, every file can be opened for shared reading (which fails while the packager is still writing), and the total size was the same in two polls in a row.
5. It is then posted to the game thread for mounting; the worker waits for the result, logs it and plays the sound.
6. After a successful mount the packages of that container are reloaded in place (since core 0.6.2).
7. A completed reload opens a 20 second window of collection requests; with nothing to reload, or a reload that did not complete, the window is 120 seconds (which goes with the older way of returning to the login screen).
8. Once the old packages have been collected, superseded older containers are unmounted and their files deleted (since core 0.7.0 / 0.7.1).

## Engine references (stock UE 5.6 source)

- `Runtime/PakFile/Private/IPlatformFilePak.cpp`
  - 4414–4504: the `Mount` / `Unmount` console commands are inside `#if !UE_BUILD_SHIPPING`.
  - 5716: `FCoreDelegates::MountPak.BindRaw(this, &FPakPlatformFile::HandleMountPakDelegate)`, after `MountAllPakFiles`.
  - 5951–5976: a file name ending in `_P.pak` adds `100 * version` to the order; the version is 1 for `name_P` and N+1 for `name_N_P`.
  - 6047–6066: mounting a pak also mounts the utoc/ucas of the same name and registers them with the package store.
  - 6384: `GetPakOrderFromPakFilePath`; a path starting with `ProjectContentDir` gives 3.
  - 6406: `HandleMountPakDelegate`.
- `Runtime/PakFile/Private/FilePackageStore.cpp:410`: containers are sorted by descending order; with equal order the one mounted later comes first.
- `Runtime/Core/Private/Windows/WindowsPlatformFile.cpp`: files are opened for reading without `FILE_SHARE_DELETE`, so a mounted file can be neither renamed nor overwritten.
- `Runtime/CoreUObject/Private/Serialization/AsyncLoading2.cpp`: `FGlobalImportStore::AddPackageRef` drops the record of a loaded package whose `UPackage` has been renamed, which allows a package of the same name to be loaded again. This is code outside the editor. The engine's own `ReloadPackages` is stripped from the game.

## The game executable (2026-10-01 build)

- Every section is called `.std` and the entry point is in the last one: a packer.
- 63% of the pages of the code section are all zero on disk and the rest is ciphertext; read-only data is plain, and the unwind table (892,503 entries) is complete.
- So strings can be checked offline, while code can only be read at run time.
- RVAs of that build, for comparison only: anchor string `0xb7bc558`, `Unmounting pak file: %s \n` at `0xb7bc608`, `_P.pak` at `0xb7bc1f8`, `UnrealWindow` at `0xb4c2028`.

## The test host

`test/host.cpp` reproduces only what the plugin relies on: the anchor strings, each referenced from its handler (the mount one from two functions), heap delegate instances holding `{object, member function}` (bound 2.5 seconds after start), mounted container files that are held open the way the engine holds them, the construct and native tables that name `CollectGarbage`, a name pool and an object table (more than 1500 objects, built 6.5 seconds after start), and an `UnrealWindow` with a message loop (created after 4 seconds). When a mount is called it checks the path, the order argument, the string length and the calling thread; a collection request has to follow on the main thread; an unmount has to name the first version, once, on the main thread. Only then does it return 0.

The host has a texture object with the root-set flag that one object points at directly and another holds through a heap array. `test\run.ps1` has the host write a `.utoc` naming just that package (`--write-utoc`) and checks that the probe report finds all three and blames no unrelated object. It then publishes a second version and checks that exactly the first is unmounted and its three files deleted, publishes a third with a lower number and checks that it is left alone, and replaces the core while the host runs.
