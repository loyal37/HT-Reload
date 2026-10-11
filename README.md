# HT Reload

A hot reload plugin for pak mods of Neverness to Everness. While the game is running, drop the three files of a mod under a new name anywhere below `HT\Content\Paks`: the plugin has the engine mount them and replaces the assets that are already loaded with the new version, in place. No restart, and no trip back to the login screen.

It is meant for iterating on a mod of your own. It is not something to ship to players with a mod.

## Risks

The game has anti-cheat. The plugin reads the game's memory, installs a message hook on the thread of the game window, calls engine functions and rewrites pointers in engine objects. It does nothing to evade detection, and whether the anti-cheat acts on an account because of it cannot be judged. Use it at your own risk, preferably on a secondary account.

A reload changes the state of the running engine. Where the plugin judges something wrongly, the game may crash or hang.

The plugin deletes files: the older versions of a mod that a new version supersedes.

## Installation

1. The game's `HT\Binaries\Win64` folder needs [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader), placed there as `dsound.dll`.
2. Put `HTReload.asi` and `HTReload.core.dll` into the same folder.
3. Start the game as usual. Once `Win64\HTReload.log` says `Ready`, it can be used.

To disable it, rename or delete `HTReload.asi`.

## Usage

1. Change the mod and package it. **While the game is open the name has to be new**: an increasing number before `_P`, for example `blackbird_1_P`, `blackbird_2_P`. Files of the same name are held by the game and cannot be overwritten.
2. Put the new `.pak`, `.utoc` and `.ucas` into the mod's folder below `HT\Content\Paks`.
3. Within a few seconds two rising tones say that the new pak is mounted, and a third, higher tone right after them says that the reload is done and the game shows the new version. The game stands still for a moment during a reload, longer for more packages.
4. A high tone followed by a lower one means the reload ran to the end but left some asset or reference as it was; a single low tone means it did not complete. Either way the details are in `Win64\HTReload.reload.txt`, and returning to the login screen and entering the game again still brings the new version in.

A higher number means a higher priority, so a new version always wins over an older one.

Once a reload is done and the old assets are collected (usually within ten seconds), the plugin has the engine unmount the older versions that the new one fully supersedes and **deletes their three files** (not to the recycle bin). Copy an old version elsewhere beforehand if you want to keep it.

- "Fully supersedes" means that every package of the old version is also in a mounted version with a higher number that was not mounted before it.
- When a reload did not complete, or old assets have not been collected, nothing is unmounted or deleted; such an old version has to be deleted by hand after the game exits.
- A new pak with a lower number than a mounted version does not take effect, and is not deleted as an old version either; the log has a note.

Do not drop a new pak in while the game shows a loading screen or changes scenes. That has not been tried.

## Known limits

- References are followed through object bodies and through array properties. References held in maps or sets, or in arrays nested inside other containers, are not found; an asset that is only reachable that way keeps its old version in use, and the reload reports itself as partly done or leaves the old version mounted.
- A mod may replace assets of the game in place, such as a texture at its original path. The game's material instances that use it are then reloaded along with it. A texture that a base material (not an instance) refers to directly is not handled.
- When a new mesh uses a different material in a slot, the override material the game made for that slot follows it. The outline material the game pairs with it does not.
- After a reload, some material slots of the character on the field are no longer dynamic materials, so effects such as hit flashes or dissolves may differ on those parts from a fresh entry. Returning to the login screen and entering again restores them.
- Instances of a mod's own widgets or blueprint classes that exist at that moment stay instances of the old class.
- Verified only on the client build of October 2026, with two character mods.

## Build, test, deploy

Needs Visual Studio 2022 with the C++ x64 toolset.

```powershell
.\build.ps1        # builds build\HTReload.asi (the shell) and build\HTReload.core.dll (the core)
.\test\run.ps1     # end-to-end test against a stand-in host; the game is not needed
.\deploy.ps1       # copies into the game's Win64 folder
```

The plugin has two parts. The shell `HTReload.asi` is loaded when the game starts and stays. The core `HTReload.core.dll` is loaded by the shell and replaced as soon as the file changes. So the core can be deployed while the game runs; only a changed shell needs the game closed first.

## The command file

Write a file `HTReload.cmd.txt` into the game's `Win64` folder, one command per line. The core runs it within a second and deletes it; the answers go to `HTReload.out.txt`. `tools\ask.ps1` does this for you.

- `plan [filter]`: lists what a reload would do, without changing anything. The filter selects the mod paks on disk whose path contains it.
- `reload [filter]`: reloads those mod paks by hand.
- `shadowed`: lists the mod paks that a newer version fully supersedes and that could be unmounted, without changing anything.
- `unmount`: unmounts those mod paks by hand and deletes their files.
- `gc [seconds]`: requests garbage collections for that long.
- `probe <label> [filter]`: reports the loaded objects that belong to those mod paks and who still references them, appended to `HTReload.probe.txt`.
- `objects`, `obj`, `mem`, `vtable`, `code`, `strref`, `astrref`, `callers`, `names`, `refs`, `arrayrefs`, `functions`, `props`: read-only views into the engine, described in `src\explore.h`.

## How it works

- Loaded by Ultimate ASI Loader; acts only inside `HTGame.exe`.
- It changes no game code and relies on no fixed addresses. The game's executable is encrypted on disk, so the plugin locates the engine's entry points at run time, by log strings, vtable positions and the shape of data structures.
- `Content\Paks` is scanned once a second. Only a pak that appears after startup and is completely written is handled; what is there at startup is left alone.
- Mounting: the engine's own handler of `FCoreDelegates::MountPak` is called on the game thread.
- Reloading:
  1. The package ids are read from the new pak's `.utoc` and matched against the engine's object table to find the old packages of the same names that are loaded.
  2. The old packages are renamed. When the loader finds a loaded package renamed, it drops its record, which allows a package of that name to be loaded again.
  3. The assets of each package are loaded again, now from the new pak, which has the highest priority.
  4. Pointers to old assets in other objects and in their array properties are replaced: plain data is rewritten; a dynamic material keeps its object and gets the new parent; a skeletal mesh component on the field is reset through the engine's "set mesh" function, so that the engine rebuilds its render state and anim instances, and its override materials are made to follow the slots of the new mesh. A material instance of the game that refers to a replaced asset is not patched but reloaded together with it, which rebuilds its render state.
  5. Garbage collections are requested, which removes the old objects.
- Steps 2 to 4 run in one task on the game thread.
- Unmounting: when the old objects are collected, the engine's own handler of `FCoreDelegates::OnUnmountPak` is called on the game thread, and the engine closes the files of the old container; once they are confirmed to be let go, they are deleted.

Project state, design notes and known issues are in `.ai-memory\`.
