# Project Overview

## Name and goal

Project name: HT Reload.

Goal: hot reload of pak mods for Neverness to Everness while the game is running. After a mod is changed and packaged again, the new version shows in the game without restarting it, which saves the restart, the login and the trip back into the scene.

## Problems it solves

- The game scans `Content\Paks` and mounts what it finds only once, at startup. Nothing mounts a container afterwards.
- The shipping build of the engine has no `Mount` / `Unmount` console commands.
- The game executable is packed and encrypted on disk, so function addresses cannot be found offline.
- Mounting alone does not change assets that are already loaded, and the game keeps a character's assets referenced even while the character is off the field.

## Intended user

The project's author, for iterating on character mods of their own. It is not meant for players and is not distributed with mods.

## Technology

- C++20, MSVC (VS 2022 17.14, toolset 14.44), static CRT, depends only on KERNEL32 / USER32
- Output is the shell `HTReload.asi` (a renamed DLL loaded by Ultimate ASI Loader) and the core `HTReload.core.dll` (loaded by the shell, replaceable while the game runs)
- Windows x64
- Build, test and deploy scripts are Windows PowerShell 5.1

## Environment

- Development directory: `E:\HT Reload`, remote repository `loyal37/HT-Reload` (created by the user; private at first, public since 2026-10-10)
- Game directory: `D:\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64`
- Game engine: a modified UE 5.6; the stock source is on this machine at `D:\ue\UE_5.6` and is used to check engine behaviour
- ASI loader: `dsound.dll` in the game's Win64 folder is Ultimate ASI Loader x64 9.7.1; it loads `.asi` files from Win64 and `Win64\plugins`
- Other ASI files there: `plugins\UniversalSigBypasser.asi` (pak signature patch, the reason mods mount at all) and `Anticensor.asi` (a near-camera fade patch, unrelated to paks)
- The game ships with the `AntiCheatExpert` anti-cheat
- Mods live in `HT\Content\Paks\MOD\<mod name>\*.pak|.utoc|.ucas`
- Packager: `D:\Neverness to Everness Mod Loader\cook\packager` (repository `loyal37/NTE-Mod-Packager`); the user has it publish straight into the game's `Paks\MOD\<mod name>` folder
- Related project: `D:\ueproject\HT` (UE editor plugins and NTE Bridge), with its own `.ai-memory`

## Core features

- Locates, at run time, the engine's `FPakPlatformFile::HandleMountPakDelegate`, the matching unmount handler and the `FPakPlatformFile` object they are bound to.
- Scans `Content\Paks` once a second and, when a new and completely written pak (with its utoc/ucas) appears, has the engine mount it on the game thread.
- Reloads in place what the game has loaded from the packages of that container: renames the old packages, loads them again, repoints references, lets the old objects be collected.
- Unmounts and deletes older versions that the new one fully supersedes.
- Sounds for success and failure; the log is `HTReload.log` next to the ASI.
- A command file gives read-only access to the running engine for diagnosis.

## Explicitly out of scope

- No anti-cheat evasion or hiding of any kind.
- No patching of game code and no inline hooks.
- Not distributed to players with mods.

## Standing constraints

- User decision: NTE Bridge is not used for now; packaging stays with the packager. Changes on the packaging side belong in the packager, not in the bridge.
- While the game runs, the three files of a mounted container are locked, so a hot publish needs a new name: an increasing number before `_P` (`name_1_P`, `name_2_P`).
- The locating code must not hard-code addresses or byte signatures; it has to survive game updates. After an update, read the log first and only then decide whether anything needs changing.
- Everything in the repository is written in English: documentation, comments, log and report messages, commit messages.
- `.ps1` scripts stay pure ASCII: Windows PowerShell 5.1 reads BOM-less scripts in the system code page.
- Run `test\run.ps1` after a change and deploy only when it passes. The core can be deployed while the game runs; the game has to be closed when the shell changed.
- **Commit after every round of changes; do not let work pile up.** The user required this on 2026-10-10, after four versions (v0.3.0 to v0.4.0, 32 files) had ended up in one commit. A round is committed as soon as its test passes. Pushing to `origin` afterwards continues the earlier request to publish the source on GitHub; the user's instruction itself only said to commit, so the push is the assistant's reading.
- Update `.ai-memory` after each commit with the exact commit, test and deploy results and the next step.
- Never write tokens, cookies, private keys or other secrets into the project memory.
