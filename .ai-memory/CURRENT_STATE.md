# Current State

## HT Reload (2026-10-11)

- **The complete in-place reload works in the game, and the user confirmed seeing the result.** After a new version is published, mesh, materials, textures and anim blueprint are replaced without a return to login, the old objects are all collected, and the older version is unmounted and deleted.
- **Deployed in the game folder: shell 0.7.2 and core 0.8.1**, in `D:\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64`:
  - shell `HTReload.asi`, SHA256 `BBE39E1BC28EA920C6BAB4D6ADBA91CD299127AF76AACF726A8214903122309A`, copied on 2026-10-11 with the game closed; in use since.
  - core `HTReload.core.dll` 0.8.1, SHA256 `A524026F4B0AA55008D1504C517460E1009DACF12235BD99A8CC2CF3B766C217`, copied at 17:35 on 2026-10-11 with the game closed. It adds the leftover report to 0.8.0 and has not run in the game yet.
- **Core 0.8.0 carried out a reload in the game** (run started 15:07 on 2026-10-11), with the FADIA mod and a slot that changed its material:
  - 9/9 packages renamed and loaded: the 4 of the mod plus 5 material instances of the game that refer to its textures.
  - The log says `Slot 2 of CharacterMesh0 has its dynamic material based on MI_player_039_fadia_a (was MI_player_039_fadia_b)`; 4 dynamic materials reparented (two of them outline materials on `outline_039_cloth`), 4 slots rewritten or reset, 4 array elements rewritten (override materials of a sequence template and the material array of another mesh, `player_039_fadia_tuoyifu`).
  - The user reported the result as fine.
  - **Not fine: 4 old packages were not collected, so `FADIA_2_P` was not unmounted.** The game was closed before this could be looked at. Something the reload does not reach still holds them; core 0.8.1 writes `HTReload.leftover.txt` in that case, which is what to read after the next reload.
- Release `v0.8.1` is the latest on GitHub (published 2026-10-11 19:12 at the user's request), pointing at `5fffcd3`, with `HTReload-0.8.1.zip`: the shell and core that are deployed (hashes above) and the README. Its notes say plainly that this core has not run in the game and that old packages stayed alive in the one run of 0.8.0. `v0.7.1` (`07d9329`) and `v0.6.2` (`f22f629`) are still there, with their original attachments.
- git: `origin` = `https://github.com/loyal37/HT-Reload`, **public** (seen through `gh` on 2026-10-10; the record before that said private, and the visibility was changed by the user, not by the assistant). Whatever is pushed, `.ai-memory` included, is visible to everyone.
- Latest code commit: `2f49348` (core 0.8.1); 0.8.0 was `c619c5e`.
- **Rule from the user: commit after every round of changes.** See the standing constraints in PROJECT_OVERVIEW.
- The git identity on this machine was corrected on 2026-10-11 at the user's request: the global email had been `loyal37@github.com`, which GitHub links to no account, and is now `loyal37@users.noreply.github.com`. Commits before `c619c5e` in this repository carry the old address and show no contributor.
- Mod folders: `Blackbird Judgment Bunny 1.1` holds `blackbird_6_P`; `FADIA` holds `FADIA_2_P` and `FADIA_3_P` (the user removed the others between runs).

## The FADIA test of 2026-10-11 (core 0.7.2, run started 12:31, PID 27288)

The FADIA mod has 4 packages: the skeletal mesh `player_039_fadia_skin` and three textures `T_player_039_fadia_01_m/_d/_id`, all at the game's own paths under `/Game/Characters/Player/039_fadia/`. It uses the game's materials.

- `FADIA_2_P` (some faces deleted): 4/4 packages renamed and loaded; the mesh changed visibly. But the three old texture packages were never collected and the old version was not unmounted.
- `FADIA_3_P` (slot 2 changed from `MI_player_039_fadia_b` to `MI_player_039_fadia_a`): only 1/1 package was found loaded, the mesh. No visible change, and again nothing unmounted.
- Cause of the first: the textures are referenced from the texture parameter arrays of the game's material instances (`MI_player_039_fadia_a` and the outline material `outline_039_cloth` for the `_d` texture). References in array storage were not followed, so the old textures stayed in use under their renamed packages and the new copies, referenced by nobody, were collected. That is also why the second reload found only the mesh.
- Cause of the second: the component holds an override material for each of its 14 slots, a dynamic material made from the mesh's material at character setup (`MID_MI_player_039_fadia_b_…` for slot 2), and an override wins over the mesh's own material. Unlike with the Blackbird mod, nothing rebuilt these after the mesh reset; there the mod's own anim blueprint appears to have done that.
- Both are addressed in core 0.8.0. That session itself cannot be repaired by another reload.

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

1. After the user's next reload, read `HTReload.leftover.txt` to find what keeps the 4 old FADIA packages alive.
2. The user asked whether their toggle blueprint and panel reload. What the Blackbird reloads show: the four blueprint packages were reloaded every time, the engine made a new anim instance from the new class, and the dynamic materials of the 14 cloth slots were rebuilt right after, which the FADIA mod (no blueprint) does not show, so that was the mod's own toggle logic running again. A reload with a changed blueprint, or with the panel open or opened earlier in the session, has still not been tried; the user was asked to test with an added toggle.
3. If the user finds effects wrong on the body, deal with the two body1 slots that have no dynamic material (`PrimitiveComponent.CreateDynamicMaterialInstance` after the mesh reset could add them, once it is clear whether the game's own bookkeeping accepts them).

Commands are sent with `tools\ask.ps1`; `tools\disasm.ps1` disassembles the bytes that the `code` command returns.
