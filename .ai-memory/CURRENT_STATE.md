# Current State

## HT Reload (2026-10-11)

- **The complete in-place reload works in the game, and the user confirmed seeing the result.** After a new version is published, mesh, materials, textures and anim blueprint are replaced without a return to login, the old objects are all collected, and the older version is unmounted and deleted.
- **Deployed in the game folder: shell and core 0.7.2**, in `D:\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64`, copied on 2026-10-11 with the game closed:
  - shell `HTReload.asi`, SHA256 `BBE39E1BC28EA920C6BAB4D6ADBA91CD299127AF76AACF726A8214903122309A`
  - core `HTReload.core.dll`, SHA256 `226C039F75A6028990588A8A9804549C5E1CDE5061EC82B49EDB5D1F415AE61B`
  - **Neither has run in the game yet.** 0.7.2 differs from 0.7.1 only in the wording of log and report messages, and the stand-in test passed, but the shell binary is new, so the next start of the game is its first run. What to check in the log: `Shell: started`, `Located:`, `Unmount: HandleUnmountPakDelegate`, `Ready:`.
- Release `v0.7.1` is the latest on GitHub (published 2026-10-10 18:42 at the user's request), pointing at `07d9329`, with `HTReload-0.7.1.zip` (shell `0F2CB234…`, core `6403822D…`, README). `v0.6.2` (`f22f629`) is still there. Nothing newer than 0.7.1 has been released.
- git: `origin` = `https://github.com/loyal37/HT-Reload`, **public** (seen through `gh` on 2026-10-10; the record before that said private, and the visibility was changed by the user, not by the assistant). Whatever is pushed, `.ai-memory` included, is visible to everyone.
- Latest code commit: `3716259` (core 0.7.2), followed by the commit that rewrote README and memory.
- **Rule from the user: commit after every round of changes.** See the standing constraints in PROJECT_OVERVIEW.
- The mod folder currently holds `blackbird_6_P` only (packaged 2026-10-10 19:12 with the packager's automatic numbering).

## What was verified in the game on 2026-10-10

Core 0.7.1, run started 18:39 (PID 13032): `blackbird_5_P` mounted at 18:41:04, reload done at 18:41:06 (26/26 packages and assets; 16 reparented, 6 rewritten, 1 mesh reset), `blackbird_4_P` unmounted and deleted at 18:41:12. Only `blackbird_5_P` was left in the mod folder afterwards and the game stayed responsive.

Core 0.7.0, run started 18:22: the unmount handler was found (`HandleUnmountPakDelegate` RVA `0x27b29d0`, size 0x59); `blackbird_5_P` mounted at 18:24:33, reload done at 18:24:35, `blackbird_4_P` unmounted at 18:24:41. That container had been mounted by the engine itself at startup, so the path the plugin passes matches the spelling the engine uses at startup. The user confirmed that the files could then be deleted.

Automatic reload after a mount (core 0.6.2): at 17:52 in the run with PID 37696, and at 18:10 in a new run after the user restarted the game (PID 9868). Both completed in under 2 seconds with 26/26. In the second, 16 dynamic materials were reparented, which shows that on a fresh entry the two body slots do have dynamic materials.

Write operations in the run started 17:19 (PID 37696), all successful:

1. `renameload 042_blackbird_body_d`: the rename returned 1 and the load returned a different, new object. The unreferenced new copy was collected seconds later; the game was unaffected.
2. `reload blackbird` at 17:34, with only `blackbird_2_P` on disk (identical content): 25/25 renamed and loaded, 16 dynamic materials reparented, 6 rewrites, the mesh of the character on the field reset. About 1.3 seconds. After collection not one package with `_HTOLD_` was left.
3. The user packaged `blackbird_4_P`, mounted automatically at 17:42; `reload blackbird` at 17:43: 26/26, 14 reparented, 6 rewrites, mesh reset. **The user confirmed seeing the change in the game.** The old packages were all collected again.

## How mounted files are held (measured 2026-10-10 with the game running)

- Probed by asking for delete access without deleting: the `.ucas` and `.utoc` of every mounted version are locked by the game and can be neither deleted nor renamed; only the 347-byte `.pak` is not locked.
- A version no longer used after a reload is locked just the same while its container is mounted.
- With the game closed everything can be deleted.

## How the game behaves around a reload

- The game's component class `HTSkeletalMeshComponentBudgeted` rebuilds materials itself when its mesh is reset:
  - The 14 cloth slots (cloth ×8, cloth2 ×6) get new dynamic materials named `MID_<parent material>_<number>`, each with an **outline material** of the same name under `/Engine/Transient` (its parent is one of the game's outline materials, for example `…/019_mint/ter_new_2/outline/cloth/player_019_mint_cloth`). The main dynamic material points at its outline material at +0x178.
  - **The 2 body slots (body1) hold the new material itself after the reset, with no dynamic material around it.** On a fresh entry they do have dynamic materials (created by another pass when the character is initialised, which a mesh reset does not repeat). The user looked at the result and found it fine, but effects driven by dynamic material parameters, such as hit flash and dissolve, were not checked on the body.
  - The other 24 slots are the game's own materials, the same in the old and new mesh; the game leaves them alone.
  - So the reparenting done first is in fact redundant for this mod (those dynamic materials are replaced by the game right after) but harmless; it is what a mod without its own mesh needs.
- The component's +0xdf8 is a native field of the game (not in reflection). It holds the new mesh pointer for a short time after a mesh reset and is then cleared.
- The anim blueprint instance is the mesh's post-process anim blueprint instance (no slot of the component points at that class). After a mesh reset the engine creates a new instance from the new class by itself; the old instance and class are then collected.
- The range of name ids (up to block 444 at the time, about 0x1BD0000) overlaps low heap addresses, and objects really are allocated that low (0x1755b80, for example). It happened in testing: the name fields (+0x18) of two `WorldPartitionRuntimeLevelStreamingCell` objects equalled the addresses of `HorizontalBox_33` and `HorizontalBox_36` in the mod's panel. Nothing was written then, because the targets were not top-level assets. Core 0.6.1 added the guard; see DECISIONS.
- The mod's toggle logic lives in the anim blueprint; the save class restores its state and the panel only displays and forwards (from the project memory of `D:\ueproject\HT`).

## Engine facts confirmed in the game

- Locating, mounting and collection requests all work. Collection entry RVA `0x4406bd0`.
- Object system: name pool Blocks RVA `0xfa36e10`; object table RVA `0xfb1af80`, 0x18-byte items; class size field at offset 0x58; GEngine global RVA `0xfcc6d90`. The object table only exists once the window has appeared.
- `UObject::Rename` = RVA `0x181e3b0`, vtable slot 46 (offset 0x170). The package class's override at RVA `0x1845670` only forwards to it.
- `UObject::ProcessEvent` = RVA `0x18aacd0`, vtable slot 76 (offset 0x260).
- `UFunction`: `FunctionFlags` 0xB0, `NumParms` 0xB4, `ParmsSize` 0xB6, `ReturnValueOffset` 0xB8, `Func` 0xD8, size 0xE0.
- `FField`: class 0x08 (an `FFieldClass` starts with its name), `Next` 0x18, name 0x20. `FProperty`: `ArrayDim` 0x30, `ElementSize` 0x34, `PropertyFlags` 0x38, `Offset_Internal` 0x44.
- Script functions:
  - `KismetSystemLibrary.LoadAsset_Blocking`: parameter block 0x30, return value at 0x28.
  - `SkeletalMeshComponent.SetSkeletalMeshAsset`: parameter block 8.
  - `MaterialInstanceDynamic.CopyParameterOverrides(UMaterialInstance*)`: parameter block 8.
  - `KismetSystemLibrary.GetFrameCount`: parameter block 8 (used by the call test).
  - `EmptyOverrideMaterials` is not a script function. `HTPlayerCharacter.ResetCharacterMesh(bool)` only resets the mesh's relative transform and is of no use here.
- Reflected property offsets: `SkinnedMeshComponent.SkeletalMesh` +0x5b0, `SkinnedAsset` +0x5b8; `MeshComponent.OverrideMaterials` +0x548 (40 entries for Blackbird); `MaterialInstance.Parent` +0x128; `HTPlayerAppearance.FashionMeshData` +0x80; `HTPlayerCharacter.CurMeshData` +0x2950, `CurPlayerAppearance` +0x2960; `Character.Mesh` +0x348.
- A character's object stays alive in the level while the character is off the field, which is why switching within the party never refreshes assets.
- Returning to the login screen and entering again refreshes everything (the older way, and the fallback when a reload does not complete).

## The packager's automatic numbering (2026-10-10, packager 2.6)

- The packager is a separate repository: `D:\Neverness to Everness Mod Loader\cook\packager` (WPF, .NET 8; `build.ps1` produces `NTE-Mod-Packager.exe` in the repository root, and that exe is tracked).
- A checkbox for automatic numbering was added, off by default and remembered. It was switched on in the user's own settings (`AutoNumber: true` in `%LOCALAPPDATA%\NTE Mod Packager\settings.json`).
- Rule (`src/NteModPackager/Services/OutputNaming.cs`): the name is typed as before; the output name is one more than the highest number the output directory holds for the same mod, and a number typed into the name is only a starting point. `blackbird_5_P` gives `blackbird_6_P` when 5 exists; `blackbird_P` starts at 1. A name not ending in `_P` is not numbered. The name is settled before the container is generated.
- Verified with `dotnet run --project tests/NteModPackager.Smoke -- naming` and with one real build driven through UI automation into a scratch output directory. The user has since packaged `blackbird_6_P` into the game folder with it.
- Local commit `de16cb0`, not pushed (a push would update the exe in the public repository, which the user has not asked for).

## Next step

1. On the next start of the game, read the log to confirm that shell and core 0.7.2 come up as before.
2. Tighten the verdict of a reload (see TASKS) once the user says who is to do it.
3. If the user finds effects wrong on the body, deal with the two body1 slots that have no dynamic material (`PrimitiveComponent.CreateDynamicMaterialInstance` after the mesh reset could add them, once it is clear whether the game's own bookkeeping accepts them).

Commands are sent with `tools\ask.ps1`; `tools\disasm.ps1` disassembles the bytes that the `code` command returns.
